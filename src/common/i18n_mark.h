// N_("text") marks a string literal as user-visible English text that is stored in a table (or handed around) and translated later with
// T(). It costs nothing at run time. tools/check_i18n.py reads it exactly like T("text"), so the literal lands in the translation checklist.
#pragma once
#define N_(s) s
