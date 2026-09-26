/* log.c - see log.h: the whole journal now lives in core/services/journal.c.
 *
 * All that is left here is the `journal_uncategorised` forwarder, because it has to
 * rebuild the variadic argument list: a `static inline` cannot pass a `...` on
 * to a function that expects its own. The other four functions of the old
 * interface are direct forwards, hence `inline` in the header.
 */
#include "log.h"

#include <stdarg.h>
#include <stdio.h>

void journal_uncategorised(const char *fmt, ...)
{
    /* The filter is tested HERE too, before formatting. Without it a legacy
     * line would pay for its `vsnprintf` twice: once to format it into this
     * buffer, once inside `journal_ecrire`. */
    if (!journal_enabled(JOURNAL_INFO, JOURNAL_CAT_LEGACY)) return;

    char line[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    /* `"%s"` and not `ligne`: the message has ALREADY been formatted, and
     * passing it back as a format would make a `%` present in the data be
     * interpreted - a machine name, a server reply. That is a format-string
     * vulnerability, and it would arrive through a string from the network. */
    journal_write(JOURNAL_INFO, JOURNAL_CAT_LEGACY, "%s", line);
}
