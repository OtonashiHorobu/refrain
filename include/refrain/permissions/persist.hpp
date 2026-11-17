// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Persistence of runtime permission changes.
//
// The policy file is operator-edited TOML: comments, table order and the
// formatting of unrelated entries are part of what the operator wrote and must
// survive a command that changes one rule. The editor therefore changes the
// smallest possible byte range of the document: it inserts one element into an
// existing `[grants]` array, removes an element from it, or adds one
// `subject = [rule]` line to the `[grants]` table. Every other byte of the file
// is copied unchanged.
//
// The supported document shape is the one the policy parser documents: a
// `[grants]` table whose values are arrays of rule strings. A document that
// expresses its grants as root-level dotted keys is refused instead of being
// rewritten into a different shape: the caller reports the refusal and keeps
// the in-memory policy unchanged.
//
// The edited text is parsed and checked against the requested change before it
// is returned, so a bug in the editor cannot produce a configuration the next
// reload would reject.

#pragma once

#include <refrain/core/error.hpp>

#include <string>
#include <string_view>

namespace refrain::permissions {

/// Add (`add == true`) or remove (`add == false`) `rule` in the `[grants]`
/// entry of `subject` and return the edited document.
///
/// Removing a rule the entry does not hold, and adding one it already holds,
/// return the document unchanged. A malformed rule, an invalid subject key, a
/// document the policy parser rejects and a document without an editable
/// `[grants]` table are errors; the caller must not write the file then.
[[nodiscard]] auto edit_grants(std::string_view text, std::string_view subject,
                               std::string_view rule, bool add)
    -> result<std::string>;

} // namespace refrain::permissions
