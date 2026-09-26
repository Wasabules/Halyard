/* ui::tr - translation of the application's text.
 *
 * Borealis already provides the foundation: JSON catalogues in
 * resources/i18n/<locale>/, automatic loading of both the system locale AND the
 * default locale at startup, hierarchical keys "file/path/key". On Switch the
 * locale follows the console language, with nothing to ask for.
 *
 * So we do not add a competing engine, only a facade:
 *
 *   - `tr("menu/continue")` instead of `brls::getStr("shadow/menu/continue")` -
 *     the catalogue prefix is implicit, which keeps call sites readable and
 *     makes it impossible to name the wrong file;
 *   - a signature that accepts formatting arguments (fmt's {});
 *   - a single place to change if the foundation ever has to be swapped out.
 *
 * Adding a language = dropping in resources/i18n/<locale>/shadow.json. Missing
 * keys fall back to en-US (Borealis' default locale), and an unknown key is
 * displayed as itself - visible in testing, never blank on screen.
 */
#pragma once

#include <string>
#include <utility>

#include <borealis/core/i18n.hpp>

namespace ui {

/* Catalogue name: a single file for the whole application. */
inline constexpr const char *I18N_CATALOG = "shadow/";

template <typename... Args>
inline std::string tr(const std::string &key, Args &&...args)
{
    return brls::getStr(I18N_CATALOG + key, std::forward<Args>(args)...);
}

}  // namespace ui
