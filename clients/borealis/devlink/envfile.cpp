/* envfile.cpp - see envfile.hpp. */

#include "envfile.hpp"
#include "devcmd.h"

#include "../../../core/services/config.h"        /* SHADOW_DATA_DIR */
#include "../../../core/services/atomic_file.h"   /* AF4 - written beside, then moved over */

#include <cstdio>
#include <cstring>

namespace devlink {
namespace {

std::string envPath()
{
    char p[256];
    std::snprintf(p, sizeof p, "%senv.txt", SHADOW_DATA_DIR);
    return std::string(p);
}

/* Reads the file as raw lines, end-of-line stripped. An absent file gives an
 * empty vector - see the header on why that is not an error. */
std::vector<std::string> readLines()
{
    std::vector<std::string> out;
    const std::string path = envPath();
    FILE *f = atomic_file_open_read(path.c_str(), "r");
    if (!f) return out;

    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        size_t n = std::strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        out.push_back(std::string(line, n));
    }
    std::fclose(f);
    return out;
}

/* The key of a `KEY=VALUE` line, or "" for a comment, a blank line or anything
 * without a separator. Those are the lines we carry over untouched. */
std::string lineKey(const std::string &l)
{
    size_t i = 0;
    while (i < l.size() && (l[i] == ' ' || l[i] == '\t')) i++;
    if (i >= l.size() || l[i] == '#') return std::string();
    const size_t eq = l.find('=', i);
    if (eq == std::string::npos) return std::string();
    return l.substr(i, eq - i);
}

}  // namespace

std::vector<std::string> envList()
{
    std::vector<std::string> out;
    for (const std::string &l : readLines()) {
        const std::string k = lineKey(l);
        if (!k.empty() && devcmd_env_key_ok(k.c_str(), k.size())) out.push_back(l);
    }
    return out;
}

bool envSet(const std::string &key, const std::string &value, std::string &why)
{
    if (!devcmd_env_key_ok(key.c_str(), key.size())) { why = "cle"; return false; }

    std::vector<std::string> lines = readLines();

    /* Replace in place when the key is already there - in place, so a file a
     * human ordered on purpose keeps its order. Only the LAST occurrence would
     * win on read, so every other one is dropped: leaving them would make the
     * file say something other than what the session will do. */
    bool placed = false;
    std::vector<std::string> out;
    out.reserve(lines.size() + 1);
    for (const std::string &l : lines) {
        if (lineKey(l) != key) { out.push_back(l); continue; }
        if (value.empty() || placed) continue;      /* removed, or a duplicate */
        out.push_back(key + "=" + value);
        placed = true;
    }
    if (!placed && !value.empty()) out.push_back(key + "=" + value);

    const std::string path = envPath();
    char tmp[256];
    FILE *f = atomic_file_open(path.c_str(), tmp, sizeof tmp);
    if (!f) { why = "ecriture"; return false; }
    for (const std::string &l : out) std::fprintf(f, "%s\n", l.c_str());

    /* `keep = 1` means "this write went fine, move it over"; it returns 1 on
     * SUCCESS - not 0, which is the convention the neighbouring code uses and
     * the one I checked against at first. Getting it backwards here would have
     * reported every successful write as a failure, and the dev machine would
     * have retried a write that had already landed.
     *
     * An env.txt that has become empty stays as an EMPTY FILE rather than being
     * removed: "no file" and "a file with no toggle" are two different things to
     * `shadow_load_env_file`, which reports them differently - and that
     * difference is what tells "I turned everything off" apart from "the channel
     * never wrote anything". */
    /* `keep` from `ferror`, like settings.cpp: a write that failed midway must
     * ABANDON rather than move a half file over the good one. */
    if (atomic_file_commit(f, tmp, path.c_str(), !std::ferror(f)) != 1) {
        why = "ecriture";
        return false;
    }
    return true;
}

}  // namespace devlink
