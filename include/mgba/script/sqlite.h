/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef M_SCRIPT_SQLITE_H
#define M_SCRIPT_SQLITE_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/script/context.h>

// Named databases opened by scripts live in <config directory>/databases, which is created on first use.
void mScriptContextAttachSQLite(struct mScriptContext* context);

CXX_GUARD_END

#endif
