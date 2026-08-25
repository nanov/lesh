#include "runtime/history_store.h"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace lesh::runtime {

namespace {

// Encodes `entry` into `out` (overwritten, not appended-to) as the one
// physical line this store's file format uses: a literal backslash becomes
// `\\`, an embedded newline becomes `\n`, every other byte is copied through
// unchanged, and the whole thing is terminated with one real newline that
// marks the entry's end. The backslash must be escaped FIRST, or a literal
// backslash immediately before a literal `n` in the original text would
// decode back as a newline nobody typed.
void encode_entry(std::string_view entry, std::string& out) {
	out.clear();
	out.reserve(entry.size() + 1);
	for (const char c : entry) {
		if (c == '\\')
			out += "\\\\";
		else if (c == '\n')
			out += "\\n";
		else
			out += c;
	}
	out += '\n';
}

// Decodes one physical line - `length` bytes starting at `line`, its
// terminating newline already excluded - back to the original entry, IN
// PLACE. Both escapes this format uses map two encoded bytes to one decoded
// byte, so the decoded text never exceeds the encoded line's length and can
// always be written starting from the same buffer it is read from, without
// the read and write cursors ever crossing.
//
// Malformed input degrades rather than fails: a backslash not followed by
// `n` or `\` - including one that is the very last byte of the line - is
// kept LITERALLY, byte for byte, exactly as it stood. A line a differently-
// escaping tool, a hand edit, or a truncated write left behind therefore
// still decodes to something and the walk continues; it is never thrown
// away and never crashes the read.
[[nodiscard]] std::string_view decode_entry(char* line, size_t length) {
	size_t read = 0;
	size_t write = 0;
	while (read < length) {
		if (line[read] == '\\' && read + 1 < length && (line[read + 1] == 'n' || line[read + 1] == '\\')) {
			line[write++] = (line[read + 1] == 'n') ? '\n' : '\\';
			read += 2;
			continue;
		}
		line[write++] = line[read++];
	}
	return {line, write};
}

} // namespace

bool history_store::append(std::string_view entry) {
	encode_entry(entry, _scratch);

	const int fd = ::open(_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (fd == -1)
		return false;

	// One write(2) call for the whole encoded line, position and all. POSIX
	// makes an O_APPEND write atomic with respect to every OTHER O_APPEND
	// writer on the same file, but only PER CALL - two write() calls for one
	// entry would open a window for a concurrent sibling shell to land its
	// own entry in between them, corrupting both into one unreadable line.
	// A short write is therefore treated as a failure rather than retried
	// piecemeal, which would reopen exactly that window.
	ssize_t written = -1;
	do {
		written = ::write(fd, _scratch.data(), _scratch.size());
	} while (written == -1 && errno == EINTR);

	::close(fd);
	return written == static_cast<ssize_t>(_scratch.size());
}

void history_store::for_each_newest_first(const std::function<void(std::string_view)>& fn) const {
	const int fd = ::open(_path.c_str(), O_RDONLY);
	if (fd == -1)
		return; // Missing (or unreadable) file: zero entries, not an error.

	struct stat st {};
	if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
		::close(fd);
		return;
	}

	std::string content(static_cast<size_t>(st.st_size), '\0');
	size_t total_read = 0;
	while (total_read < content.size()) {
		const ssize_t n = ::read(fd, content.data() + total_read, content.size() - total_read);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break; // Failed read: iterate whatever made it in before the error.
		}
		if (n == 0)
			break; // File shrank under us (rotated/truncated elsewhere) - stop here.
		total_read += static_cast<size_t>(n);
	}
	::close(fd);
	content.resize(total_read);
	if (content.empty())
		return;

	// Collect each physical line's [start, end) span, front to back - `end`
	// excludes the line's own terminating '\n' when it has one. A final
	// segment with no terminator (a write cut off mid-entry, e.g. by a
	// crash) is still collected, as the newest, malformed entry - dropped
	// data would be a worse answer than a best-effort decode.
	std::vector<std::pair<size_t, size_t>> lines;
	size_t start = 0;
	for (size_t i = 0; i < content.size(); ++i) {
		if (content[i] == '\n') {
			lines.emplace_back(start, i);
			start = i + 1;
		}
	}
	if (start < content.size())
		lines.emplace_back(start, content.size());

	// "Newest first" falls straight out of "append-only": the last line
	// written is the last line in the file, so walking the spans back to
	// front is the whole algorithm.
	for (auto it = lines.rbegin(); it != lines.rend(); ++it)
		fn(decode_entry(content.data() + it->first, it->second - it->first));
}

std::optional<std::string> history_store::default_path() {
	const char* home = std::getenv("HOME");
	if (home == nullptr || home[0] == '\0')
		return std::nullopt;

	std::string path{home};
	if (path.back() != '/')
		path += '/';
	path += ".lesh_history";
	return path;
}

} // namespace lesh::runtime
