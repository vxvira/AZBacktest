// plug n play csv toolkit, zero copy mmapped reading plus buffered writing
#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace csv {

/// @brief how a buffer gets split into rows and fields
struct Options {
    char delimiter = ',';
    bool header    = true;  // first line is a header, next() never hands it back
    bool quoted    = false; // honour "..." so delimiters inside quotes dont split the field,
                            // off by default since the plain memchr path is a lot faster.
                            // newlines inside quotes aren't supported either way
};

/// @brief how Writer formats what it writes
struct WriteOptions {
    char delimiter      = ',';
    bool append         = false; // add to the end of an existing file instead of truncating it
    int  precision      = -1;    // digits after the point for floats, -1 = shortest form that round trips
    const char* newline = "\n";
};

namespace detail {

/// @brief find first '\n' in [p, end), returns end if none found
/// memchr on modern glibc / MSVC uses SIMD (SSE2/AVX) for the scan so
/// this is basically free compared to looping byte-by-byte
inline const char* findEOL(const char* p, const char* end) {
    if (p >= end) return end;
    const void* nl = std::memchr(p, '\n', static_cast<std::size_t>(end - p));
    return nl ? static_cast<const char*>(nl) : end;
}

/// @brief end of the field starting at p, the next delimiter or `end`
/// in quoted mode a delimiter between quotes doesnt count, "" just toggles
/// twice so escaped quotes fall out for free
inline const char* fieldEnd(const char* p, const char* end, char delim, bool quoted) {
    if (p >= end) return end;
    if (!quoted) {
        const void* c = std::memchr(p, delim, static_cast<std::size_t>(end - p));
        return c ? static_cast<const char*>(c) : end;
    }
    bool inQuotes = false;
    for (; p < end; ++p) {
        if (*p == '"') inQuotes = !inQuotes;
        else if (*p == delim && !inQuotes) return p;
    }
    return end;
}

inline std::string_view trimSpaces(std::string_view v) {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
    while (!v.empty() && (v.back()  == ' ' || v.back()  == '\t')) v.remove_suffix(1);
    return v;
}

template <typename T> struct isOptional : std::false_type {};
template <typename T> struct isOptional<std::optional<T>> : std::true_type {};

template <typename> inline constexpr bool alwaysFalse = false;

} // namespace detail

/// @brief strip the surrounding quotes off a field and collapse "" back into ",
/// fields that arent quoted come back unchanged
inline std::string unquote(std::string_view f) {
    if (f.size() < 2 || f.front() != '"' || f.back() != '"') return std::string(f);
    std::string out;
    out.reserve(f.size() - 2);
    for (std::size_t i = 1; i + 1 < f.size(); ++i) {
        out += f[i];
        if (f[i] == '"' && i + 2 < f.size() && f[i + 1] == '"') ++i;
    }
    return out;
}

/// @brief parse `v` into `dst`, leaving dst alone (and returning false) when v
/// is empty or doesnt parse, so whatever default was set beforehand survives.
/// numbers go through from_chars (surrounding spaces ignored), bools take
/// 1/0/true/false, strings and string_views are just copied/pointed at
template <typename T>
inline bool parse(std::string_view v, T& dst) {
    if constexpr (std::is_same_v<T, std::string>) {
        dst = std::string(v);
        return true;
    } else if constexpr (std::is_same_v<T, std::string_view>) {
        dst = v;
        return true;
    } else {
        v = detail::trimSpaces(v);
        if (v.empty()) return false;
        if constexpr (std::is_same_v<T, bool>) {
            if (v == "1" || v == "true"  || v == "True"  || v == "TRUE")  { dst = true;  return true; }
            if (v == "0" || v == "false" || v == "False" || v == "FALSE") { dst = false; return true; }
            return false;
        } else if constexpr (std::is_arithmetic_v<T>) {
            T tmp{};
            auto r = std::from_chars(v.data(), v.data() + v.size(), tmp);
            if (r.ec != std::errc()) return false;
            dst = tmp;
            return true;
        } else {
            static_assert(detail::alwaysFalse<T>, "csv::parse: unsupported type");
        }
    }
}

/// @brief parse `v` as a T, `fallback` when it's empty or doesnt parse
template <typename T>
inline T as(std::string_view v, T fallback = T{}) {
    parse(v, fallback);
    return fallback;
}

/// @brief one line of a csv, views into whatever buffer it came from, so it's
/// only valid as long as that buffer is. a trailing '\r' is dropped so CRLF
/// files behave the same as LF ones
class Row {
private:
    const char* _start = nullptr;
    const char* _end   = nullptr;
    char _delim  = ',';
    bool _quoted = false;

public:
    Row() = default;
    Row(const char* start, const char* end, char delim = ',', bool quoted = false)
        : _start(start), _end(end), _delim(delim), _quoted(quoted) {
        if (_end > _start && _end[-1] == '\r') --_end;
    }
    explicit Row(std::string_view line, const Options& opt = {})
        : Row(line.data(), line.data() + line.size(), opt.delimiter, opt.quoted) {}

    std::string_view raw() const { return {_start, static_cast<std::size_t>(_end - _start)}; }
    bool empty()  const { return _start == _end; }
    bool quoted() const { return _quoted; }
    char delimiter() const { return _delim; }

    /// @brief the col-th (0-indexed) field, empty if col is negative or past
    /// the end of the row. quoted fields come back with their quotes, see text()
    std::string_view operator[](int col) const {
        if (col < 0) return {};
        const char* p = _start;
        for (int i = 0; i < col; ++i) {
            const char* d = detail::fieldEnd(p, _end, _delim, _quoted);
            if (d >= _end) return {};
            p = d + 1;
        }
        const char* e = detail::fieldEnd(p, _end, _delim, _quoted);
        return {p, static_cast<std::size_t>(e - p)};
    }

    /// @brief single pass extraction of several columns, out[i] gets the field
    /// at cols[i]. cols can be in any order (it stops at whichever is largest),
    /// a negative cols[i] leaves out[i] untouched, and so does a column the row
    /// is too short to have
    /// @param cols n column indices, any order, negatives meaning "not wanted"
    /// @param out  caller-owned array of n views, only the ones that matched get written
    void fields(const int* cols, int n, std::string_view* out) const {
        int maxCol = -1;
        for (int i = 0; i < n; ++i) if (cols[i] > maxCol) maxCol = cols[i];
        if (maxCol < 0) return;

        const char* p = _start;
        for (int col = 0; col <= maxCol; ++col) {
            const char* e = detail::fieldEnd(p, _end, _delim, _quoted);
            for (int i = 0; i < n; ++i)
                if (cols[i] == col) out[i] = std::string_view(p, static_cast<std::size_t>(e - p));
            if (e >= _end) return;
            p = e + 1;
        }
    }

    /// @brief how many fields the row has, 0 for a blank line
    int size() const {
        if (empty()) return 0;
        int n = 1;
        for (const char* p = _start; (p = detail::fieldEnd(p, _end, _delim, _quoted)) < _end; ++p) ++n;
        return n;
    }

    /// @brief every field in order
    std::vector<std::string_view> split() const {
        std::vector<std::string_view> out;
        if (empty()) return out;
        const char* p = _start;
        while (true) {
            const char* e = detail::fieldEnd(p, _end, _delim, _quoted);
            out.emplace_back(p, static_cast<std::size_t>(e - p));
            if (e >= _end) break;
            p = e + 1;
        }
        return out;
    }

    /// @brief field `col` parsed as a T, `fallback` when it's missing or doesnt parse
    template <typename T>
    T get(int col, T fallback = T{}) const { return as<T>((*this)[col], fallback); }

    /// @brief parse field `col` into dst, untouched when it's missing or doesnt parse
    template <typename T>
    bool parse(int col, T& dst) const { return ::csv::parse((*this)[col], dst); }

    /// @brief field `col` as an owned string with any quoting undone
    std::string text(int col) const {
        std::string_view f = (*this)[col];
        return _quoted ? unquote(f) : std::string(f);
    }
};

/// @brief read only memory map of a whole file, move only. an empty file maps
/// to an empty buffer instead of failing (the OS calls refuse zero length maps)
class MappedFile {
private:
    const char* _data = nullptr;
    std::size_t _size = 0;
#ifdef _WIN32
    HANDLE _file = INVALID_HANDLE_VALUE;
    HANDLE _map  = nullptr;
#else
    int _fd = -1;
#endif

    void _release() noexcept {
#ifdef _WIN32
        if (_data)                         UnmapViewOfFile(_data);
        if (_map)                          CloseHandle(_map);
        if (_file != INVALID_HANDLE_VALUE) CloseHandle(_file);
        _file = INVALID_HANDLE_VALUE;
        _map  = nullptr;
#else
        if (_data)     munmap(const_cast<char*>(_data), _size);
        if (_fd != -1) ::close(_fd);
        _fd = -1;
#endif
        _data = nullptr;
        _size = 0;
    }

public:
    MappedFile() = default;

    explicit MappedFile(const std::string& path) {
#ifdef _WIN32
        _file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                            nullptr);
        if (_file == INVALID_HANDLE_VALUE)
            throw std::runtime_error("csv: open failed: " + path);
        LARGE_INTEGER sz;
        if (!GetFileSizeEx(_file, &sz)) {
            _release();
            throw std::runtime_error("csv: size query failed: " + path);
        }
        _size = static_cast<std::size_t>(sz.QuadPart);
        if (_size == 0) return;
        _map = CreateFileMappingA(_file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!_map) {
            _release();
            throw std::runtime_error("csv: file mapping failed: " + path);
        }
        void* p = MapViewOfFile(_map, FILE_MAP_READ, 0, 0, 0);
        if (!p) {
            _release();
            throw std::runtime_error("csv: map view failed: " + path);
        }
        _data = static_cast<const char*>(p);
#else
        _fd = ::open(path.c_str(), O_RDONLY);
        if (_fd == -1)
            throw std::runtime_error("csv: open failed: " + path);
        struct stat st;
        if (fstat(_fd, &st) == -1) {
            _release();
            throw std::runtime_error("csv: stat failed: " + path);
        }
        _size = static_cast<std::size_t>(st.st_size);
        if (_size == 0) return;
        void* p = mmap(nullptr, _size, PROT_READ, MAP_PRIVATE, _fd, 0);
        if (p == MAP_FAILED) {
            _size = 0;
            _release();
            throw std::runtime_error("csv: mmap failed: " + path);
        }
        madvise(p, _size, MADV_SEQUENTIAL);
        _data = static_cast<const char*>(p);
#endif
    }

    ~MappedFile() { _release(); }

    MappedFile(MappedFile&& o) noexcept { *this = std::move(o); }
    MappedFile& operator=(MappedFile&& o) noexcept {
        if (this == &o) return *this;
        _release();
        _data = o._data; _size = o._size;
        o._data = nullptr; o._size = 0;
#ifdef _WIN32
        _file = o._file; _map = o._map;
        o._file = INVALID_HANDLE_VALUE; o._map = nullptr;
#else
        _fd = o._fd;
        o._fd = -1;
#endif
        return *this;
    }

    // copying would double-unmap
    MappedFile(const MappedFile&)            = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    const char* data() const { return _data ? _data : ""; }
    std::size_t size() const { return _size; }
    std::string_view view() const { return {data(), _size}; }
};

/// @brief streaming row cursor over a raw byte range it doesnt own (mmapped
/// file, string, whatever). rows are views into that range. lines are taken
/// literally, so a blank line comes back as an empty Row rather than being
/// skipped, which keeps row numbers and seekRow lined up with the file
class Reader {
private:
    const char* _base = nullptr;
    const char* _cur  = nullptr;
    const char* _end  = nullptr;
    Options _opt;
    bool _consumedHeader = false;

    // start of every data row, built lazily the first time something seeks by row
    std::vector<const char*> _lineIndex;
    bool _indexBuilt = false;

    const char* _afterLine(const char* p) const {
        const char* eol = detail::findEOL(p, _end);
        return (eol < _end) ? eol + 1 : _end;
    }

    const char* _dataStart() const { return _opt.header ? _afterLine(_base) : _base; }

    Row _rowAt(const char* line) const {
        return Row(line, detail::findEOL(line, _end), _opt.delimiter, _opt.quoted);
    }

    void _skipHeaderOnce() {
        if (_consumedHeader) return;
        if (_opt.header) _cur = _afterLine(_cur);
        _consumedHeader = true;
    }

    void _buildIndex() {
        if (_indexBuilt) return;
        _indexBuilt = true;
        for (const char* p = _dataStart(); p < _end; p = _afterLine(p)) _lineIndex.push_back(p);
    }

public:
    Reader() = default;
    Reader(const char* data, std::size_t size, Options opt = {})
        : _base(data), _cur(data), _end(data + size), _opt(opt) {}
    explicit Reader(std::string_view buf, Options opt = {})
        : Reader(buf.data(), buf.size(), opt) {}

    const Options& options() const { return _opt; }
    std::string_view buffer() const { return {_base, static_cast<std::size_t>(_end - _base)}; }

    /// @brief the header row, empty when Options::header is off
    Row header() const {
        if (!_opt.header || _base >= _end) return Row(nullptr, nullptr, _opt.delimiter, _opt.quoted);
        return _rowAt(_base);
    }

    /// @brief index of the header column called `name`, -1 if there isn't one
    int column(std::string_view name) const {
        Row h = header();
        std::vector<std::string_view> names = h.split();
        for (std::size_t i = 0; i < names.size(); ++i) {
            std::string_view f = detail::trimSpaces(names[i]);
            if (f == name || (_opt.quoted && unquote(f) == name)) return static_cast<int>(i);
        }
        return -1;
    }

    /// @brief next data row (header skipped on first call), nullopt at EOF
    std::optional<Row> next() {
        _skipHeaderOnce();
        if (_cur >= _end) return std::nullopt;
        Row r = _rowAt(_cur);
        _cur = _afterLine(_cur);
        return r;
    }

    /// @brief the row next() would return, without consuming it
    std::optional<Row> peek() {
        _skipHeaderOnce();
        if (_cur >= _end) return std::nullopt;
        return _rowAt(_cur);
    }

    /// @brief skip past one row without splitting it, false at EOF
    bool skip() {
        _skipHeaderOnce();
        if (_cur >= _end) return false;
        _cur = _afterLine(_cur);
        return true;
    }

    bool atEnd() {
        _skipHeaderOnce();
        return _cur >= _end;
    }

    /// @brief cursor position in bytes from the start of the buffer, feed it
    /// back into seekTo to come back here later
    std::size_t byteOffset() const { return static_cast<std::size_t>(_cur - _base); }
    void seekTo(std::size_t off) {
        _cur = _base + std::min(off, static_cast<std::size_t>(_end - _base));
        _consumedHeader = true;
    }

    /// @brief put the cursor on the row-th data row (0-indexed, header not
    /// counted), anything out of range parks it at EOF
    void seekRow(long long row) {
        _buildIndex();
        _consumedHeader = true;
        _cur = (row >= 0 && static_cast<std::size_t>(row) < _lineIndex.size())
            ? _lineIndex[static_cast<std::size_t>(row)] : _end;
    }

    /// @brief the row-th data row without moving the cursor, nullopt if out of range
    std::optional<Row> rowAt(long long row) {
        _buildIndex();
        if (row < 0 || static_cast<std::size_t>(row) >= _lineIndex.size()) return std::nullopt;
        return _rowAt(_lineIndex[static_cast<std::size_t>(row)]);
    }

    /// @brief number of data rows, header not counted. first call is a full scan
    std::size_t rowCount() {
        _buildIndex();
        return _lineIndex.size();
    }

    /// @brief back to the very start, header included
    void rewind() {
        _cur = _base;
        _consumedHeader = false;
    }
};

namespace detail {
// owns the map so it's constructed before the Reader base that points into it
struct MapHolder {
    MappedFile _map;
    explicit MapHolder(const std::string& path) : _map(path) {}
};
} // namespace detail

/// @brief Reader over a memory mapped file it owns, the plug n play version.
/// rows stay valid for as long as the FileReader is alive
class FileReader : private detail::MapHolder, public Reader {
public:
    explicit FileReader(const std::string& path, Options opt = {})
        : detail::MapHolder(path), Reader(_map.data(), _map.size(), opt) {}
};

/// @brief buffered csv writer. writes to a file, or with the default/options
/// only constructor to an in-memory string you read back through str().
/// text that contains the delimiter, a quote or a newline gets quoted, numbers
/// go through to_chars, std::optional writes a blank cell when it's empty
class Writer {
private:
    std::FILE* _f = nullptr;
    WriteOptions _opt;
    std::string _buf;
    bool _rowOpen = false; // something's been written on this row, next field needs a delimiter
    std::size_t _rows = 0;
    static constexpr std::size_t _flushAt = 1 << 16;

    void _sep() {
        if (_rowOpen) _buf += _opt.delimiter;
        _rowOpen = true;
    }

    void _putText(std::string_view s) {
        bool needsQuotes = false;
        for (char c : s)
            if (c == _opt.delimiter || c == '"' || c == '\n' || c == '\r') { needsQuotes = true; break; }
        if (!needsQuotes) { _buf.append(s.data(), s.size()); return; }
        _buf += '"';
        for (char c : s) {
            if (c == '"') _buf += '"';
            _buf += c;
        }
        _buf += '"';
    }

    template <typename T>
    void _put(const T& v) {
        using U = std::decay_t<T>;
        if constexpr (detail::isOptional<U>::value) {
            if (v) _put(*v);
        } else if constexpr (std::is_same_v<U, bool>) {
            _buf += v ? "true" : "false";
        } else if constexpr (std::is_same_v<U, char>) {
            _putText(std::string_view(&v, 1));
        } else if constexpr (std::is_floating_point_v<U>) {
            char tmp[128];
            std::to_chars_result r = _opt.precision < 0
                ? std::to_chars(tmp, tmp + sizeof(tmp), v)
                : std::to_chars(tmp, tmp + sizeof(tmp), v, std::chars_format::fixed, _opt.precision);
            // fixed notation on something huge can overflow the buffer, shortest never does
            if (r.ec != std::errc()) r = std::to_chars(tmp, tmp + sizeof(tmp), v);
            _buf.append(tmp, r.ptr);
        } else if constexpr (std::is_integral_v<U>) {
            char tmp[32];
            std::to_chars_result r = std::to_chars(tmp, tmp + sizeof(tmp), v);
            _buf.append(tmp, r.ptr);
        } else if constexpr (std::is_same_v<U, const char*> || std::is_same_v<U, char*>) {
            if (v) _putText(v);
        } else if constexpr (std::is_convertible_v<const U&, std::string_view>) {
            _putText(std::string_view(v));
        } else {
            static_assert(detail::alwaysFalse<U>, "csv::Writer: unsupported field type");
        }
    }

public:
    /// @brief in-memory writer, nothing touches disk, read it back with str()
    Writer() = default;
    explicit Writer(WriteOptions opt) : _opt(opt) {}

    /// @brief writer to `path`, truncating it unless opt.append is set
    explicit Writer(const std::string& path, WriteOptions opt = {}) : _opt(opt) {
        _f = std::fopen(path.c_str(), opt.append ? "ab" : "wb");
        if (!_f) throw std::runtime_error("csv: could not open " + path + " for writing");
    }

    ~Writer() { close(); }

    Writer(Writer&& o) noexcept { *this = std::move(o); }
    Writer& operator=(Writer&& o) noexcept {
        if (this == &o) return *this;
        close();
        _f = o._f; _opt = o._opt; _buf = std::move(o._buf);
        _rowOpen = o._rowOpen; _rows = o._rows;
        o._f = nullptr; o._buf.clear(); o._rowOpen = false; o._rows = 0;
        return *this;
    }

    Writer(const Writer&)            = delete;
    Writer& operator=(const Writer&) = delete;

    /// @brief append one field to the current row
    template <typename T>
    Writer& field(const T& v) {
        _sep();
        _put(v);
        return *this;
    }

    /// @brief append one field verbatim, no quoting or escaping, for passing
    /// through text that's already valid csv
    Writer& rawField(std::string_view v) {
        _sep();
        _buf.append(v.data(), v.size());
        return *this;
    }

    /// @brief finish the current row, an empty row writes a blank line
    Writer& endRow() {
        _buf += _opt.newline;
        _rowOpen = false;
        ++_rows;
        if (_f && _buf.size() >= _flushAt) flush();
        return *this;
    }

    /// @brief write a whole row, one field per argument, e.g. row("ESM5", 5000.25, 3)
    template <typename... Ts>
    Writer& row(const Ts&... vs) {
        (field(vs), ...);
        return endRow();
    }

    /// @brief copy a row read through csv::Reader across. quoted rows keep their
    /// quoting as is, plain ones get quoted only where this writer's delimiter needs it
    Writer& row(const Row& r) {
        for (std::string_view f : r.split()) {
            if (r.quoted()) rawField(f);
            else            field(f);
        }
        return endRow();
    }

    /// @brief one row from any range, e.g. a std::vector<double>
    template <typename It>
    Writer& rowFrom(It begin, It end) {
        for (; begin != end; ++begin) field(*begin);
        return endRow();
    }
    template <typename Container>
    Writer& rowFrom(const Container& c) { return rowFrom(std::begin(c), std::end(c)); }

    Writer& header(std::initializer_list<std::string_view> names) {
        for (std::string_view n : names) field(n);
        return endRow();
    }

    /// @brief push buffered rows out to the file, no-op for in-memory writers
    void flush() {
        if (!_f) return;
        if (!_buf.empty()) {
            std::fwrite(_buf.data(), 1, _buf.size(), _f);
            _buf.clear();
        }
        std::fflush(_f);
    }

    /// @brief finish any half written row, flush and close the file. the
    /// destructor does this too, call it yourself to read the file back early
    void close() {
        if (_rowOpen) endRow();
        if (!_f) return;
        flush();
        std::fclose(_f);
        _f = nullptr;
    }

    bool isOpen() const { return _f != nullptr; }
    std::size_t rowsWritten() const { return _rows; }

    /// @brief everything written so far, for in-memory writers. on a file writer
    /// it's just whatever hasn't been flushed yet
    const std::string& str() const { return _buf; }
};

/// @brief dump columns side by side, one container per column, e.g.
///   csv::writeColumns("equity.csv", {"ts", "equity"}, ts, equity);
/// columns can be different lengths, the short ones leave their cells blank
/// once they run out. pass {} for names to skip the header
template <typename... Cols>
inline void writeColumns(const std::string& path, std::initializer_list<std::string_view> names,
                         const Cols&... cols) {
    Writer w(path);
    if (names.size()) w.header(names);
    std::size_t rows = 0;
    ((rows = std::max(rows, static_cast<std::size_t>(cols.size()))), ...);
    for (std::size_t i = 0; i < rows; ++i) {
        ((i < cols.size() ? (void)w.field(cols[i]) : (void)w.field("")), ...);
        w.endRow();
    }
}

namespace detail {
template <typename T>
inline std::vector<T> readColumnFrom(Reader& in, int col, T fallback) {
    static_assert(!std::is_same_v<T, std::string_view>,
                  "csv::readColumn: string_views would dangle once the file closes, use std::string");
    std::vector<T> out;
    while (auto r = in.next()) {
        if constexpr (std::is_same_v<T, std::string>) out.push_back(r->text(col));
        else                                          out.push_back(r->get<T>(col, fallback));
    }
    return out;
}
} // namespace detail

/// @brief read one column of a file into a vector, rows where it's missing or
/// doesnt parse get `fallback`
template <typename T>
inline std::vector<T> readColumn(const std::string& path, int col, Options opt = {}, T fallback = T{}) {
    FileReader in(path, opt);
    return detail::readColumnFrom<T>(in, col, fallback);
}

/// @brief readColumn by header name, throws if the header doesnt have it
template <typename T>
inline std::vector<T> readColumn(const std::string& path, std::string_view name, Options opt = {}, T fallback = T{}) {
    FileReader in(path, opt);
    int col = in.column(name);
    if (col < 0) throw std::runtime_error("csv: no column named " + std::string(name) + " in " + path);
    return detail::readColumnFrom<T>(in, col, fallback);
}

} // namespace csv
