// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/command/dispatcher.hpp>

namespace refrain::command {

/// Register the `/example` command family.
///
/// The family is a registration reference and an end-to-end fixture: a single
/// command chain uses every typed parser the framework ships, so parsing, the
/// client tree and completion are exercisable without a gameplay subsystem. It
/// is deliberately not part of the gameplay command tree; a server registers
/// the commands it actually implements.
void register_parser_examples(dispatcher &into);

} // namespace refrain::command
