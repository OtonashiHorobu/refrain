// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <schema/parser.hpp>

#include <schema/lexer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::schema {

auto dialect_name(dialect value) noexcept -> const char * {
    switch (value) {
    case dialect::rsc:
        return "RSC";
    case dialect::ridl:
        return "RIDL";
    }
    return "schema";
}

auto declaration_kind_name(declaration_kind kind) noexcept -> const char * {
    switch (kind) {
    case declaration_kind::structure:
        return "struct";
    case declaration_kind::enumeration:
        return "enum";
    case declaration_kind::flags:
        return "flags";
    case declaration_kind::packet:
        return "packet";
    case declaration_kind::record:
        return "record";
    case declaration_kind::handle:
        return "handle";
    case declaration_kind::service:
        return "service";
    case declaration_kind::event:
        return "event";
    }
    return "declaration";
}

namespace {

[[nodiscard]] auto is_declaration_keyword(std::string_view text) noexcept
    -> bool {
    return text == "struct" || text == "enum" || text == "flags" ||
           text == "packet" || text == "record" || text == "handle" ||
           text == "service" || text == "event";
}

[[nodiscard]] auto is_integer_type_name(std::string_view text) noexcept
    -> bool {
    return text == "u8" || text == "u16" || text == "u32" || text == "u64" ||
           text == "i8" || text == "i16" || text == "i32" || text == "i64";
}

class parser_impl {
  public:
    parser_impl(dialect kind, std::string path, std::string_view source,
                diagnostics &sink)
        : kind_{kind}, path_{std::move(path)}, lexer_{source, path_, sink},
          diagnostics_{sink} {
        advance();
    }

    [[nodiscard]] auto parse() -> std::optional<source_unit> {
        source_unit unit;
        unit.kind = kind_;
        unit.path = path_;

        if (!at_identifier("module")) {
            error_at(current_, "expected 'module <name>;' at the start of the "
                               "file");
            return std::nullopt;
        }
        unit.module = parse_module();
        if (unit.module.empty()) {
            return std::nullopt;
        }

        if (!at_identifier("version")) {
            error_at(current_, "expected 'version <n>;' after the module "
                               "declaration");
            return std::nullopt;
        }
        unit.version = parse_version();
        if (unit.version.empty()) {
            return std::nullopt;
        }

        for (;;) {
            auto documentation{parse_documentation()};
            if (at(token_kind::end_of_file)) {
                break;
            }
            if (at_identifier("import")) {
                unit.imports.push_back(parse_import());
                continue;
            }
            auto attributes{parse_attributes()};
            auto parsed{parse_declaration(std::move(documentation))};
            if (!parsed.has_value()) {
                synchronize();
                continue;
            }
            parsed->attributes = std::move(attributes);
            unit.declarations.push_back(std::move(*parsed));
        }

        return unit;
    }

  private:
    [[nodiscard]] auto location() const -> source_location {
        return source_location{path_, current_.line, current_.column};
    }

    void advance() { current_ = lexer_.next(); }

    [[nodiscard]] auto at(token_kind kind) const noexcept -> bool {
        return current_.kind == kind;
    }

    [[nodiscard]] auto at_identifier(std::string_view text) const noexcept
        -> bool {
        return current_.kind == token_kind::identifier && current_.text == text;
    }

    void error_at(const token &where, std::string message) const {
        diagnostics_.error(source_location{path_, where.line, where.column},
                           std::move(message));
    }

    void error_here(std::string message) const {
        error_at(current_, std::move(message));
    }

    /// Consume `kind` when it matches; otherwise report and consume the
    /// offending token (except at EOF) to keep the parser moving.
    auto expect(token_kind kind, std::string_view what) -> token {
        if (at(kind)) {
            auto matched{std::move(current_)};
            advance();
            return matched;
        }
        error_here(std::format("expected {}, found {}", what,
                               token_kind_name(current_.kind)));
        auto fallback{token{}};
        if (!at(token_kind::end_of_file)) {
            fallback = std::move(current_);
            advance();
        }
        return fallback;
    }

    [[nodiscard]] auto expect_identifier(std::string_view what) -> token {
        return expect(token_kind::identifier, what);
    }

    auto consume(token_kind kind) -> bool {
        if (!at(kind)) {
            return false;
        }
        advance();
        return true;
    }

    [[nodiscard]] auto documentation_or_empty() -> std::string {
        return parse_documentation();
    }

    [[nodiscard]] auto parse_documentation() -> std::string {
        auto text{std::string{}};
        while (at(token_kind::doc_comment)) {
            if (!text.empty()) {
                text += '\n';
            }
            text += current_.text;
            advance();
        }
        return text;
    }

    [[nodiscard]] auto parse_module() -> std::string {
        advance(); // 'module'
        auto name{expect_identifier("module name").text};
        while (consume(token_kind::colon_colon)) {
            name += "::";
            name += expect_identifier("module path segment").text;
        }
        expect(token_kind::semicolon, "';' after module declaration");
        return name;
    }

    [[nodiscard]] auto parse_version() -> std::string {
        advance(); // 'version'
        auto text{std::string{}};
        if (at(token_kind::integer) || at(token_kind::floating)) {
            text = current_.text;
            advance();
        } else {
            error_here("expected a version number after 'version'");
        }
        expect(token_kind::semicolon, "';' after version");
        return text;
    }

    [[nodiscard]] auto parse_import() -> std::string {
        advance(); // 'import'
        auto name{expect_identifier("imported module name").text};
        while (consume(token_kind::colon_colon)) {
            name += "::";
            name += expect_identifier("imported module path segment").text;
        }
        expect(token_kind::semicolon, "';' after the import");
        return name;
    }

    [[nodiscard]] auto parse_attributes() -> std::vector<attribute> {
        auto attributes{std::vector<attribute>{}};
        while (at(token_kind::attribute)) {
            attribute attr;
            attr.location = location();
            advance(); // '#['
            attr.name = expect_identifier("attribute name").text;
            if (consume(token_kind::l_paren)) {
                for (;;) {
                    attribute_argument argument;
                    // RIDL attributes may name their arguments
                    // (`#[schedule(interval=1, phase=pre_tick)]`); RSC
                    // attributes stay positional so the data schema grammar is
                    // unchanged. An identifier is either a value or an argument
                    // name followed by '='; consuming it first is equivalent to
                    // one token of lookahead here.
                    if (kind_ == dialect::ridl && at(token_kind::identifier)) {
                        auto name{current_.text};
                        advance();
                        if (consume(token_kind::equals)) {
                            argument.name = std::move(name);
                            if (!parse_attribute_value(argument)) {
                                error_here("expected an attribute argument "
                                           "value after '='");
                                break;
                            }
                        } else {
                            argument.kind = attribute_argument_kind::identifier;
                            argument.text = std::move(name);
                        }
                    } else if (!parse_attribute_value(argument)) {
                        error_here("expected an attribute argument");
                        break;
                    }
                    attr.arguments.push_back(std::move(argument));
                    if (!consume(token_kind::comma)) {
                        break;
                    }
                }
                expect(token_kind::r_paren, "')' after attribute arguments");
            }
            expect(token_kind::r_bracket, "']' closing the attribute");
            attributes.push_back(std::move(attr));
        }
        return attributes;
    }

    /// Read one positional attribute value into `argument` and consume it.
    /// Returns false (without consuming) for a token that cannot be a value.
    auto parse_attribute_value(attribute_argument &argument) -> bool {
        if (at(token_kind::string)) {
            argument.kind = attribute_argument_kind::string;
        } else if (at(token_kind::integer)) {
            argument.kind = attribute_argument_kind::integer;
        } else if (at(token_kind::floating)) {
            argument.kind = attribute_argument_kind::floating;
        } else if (at(token_kind::identifier)) {
            argument.kind = attribute_argument_kind::identifier;
        } else {
            return false;
        }
        argument.text = current_.text;
        advance();
        return true;
    }

    [[nodiscard]] auto parse_declaration(std::string documentation)
        -> std::optional<declaration> {
        if (at(token_kind::identifier) &&
            is_declaration_keyword(current_.text)) {
            const auto keyword{current_.text};
            if (keyword == "struct") {
                return parse_structure(declaration_kind::structure,
                                       std::move(documentation));
            }
            if (keyword == "record") {
                return parse_structure(declaration_kind::record,
                                       std::move(documentation));
            }
            if (keyword == "packet") {
                return parse_packet(std::move(documentation));
            }
            if (keyword == "enum") {
                return parse_enum_like(declaration_kind::enumeration,
                                       std::move(documentation));
            }
            if (keyword == "flags") {
                return parse_enum_like(declaration_kind::flags,
                                       std::move(documentation));
            }
            if (keyword == "handle") {
                return parse_handle(std::move(documentation));
            }
            if (keyword == "event") {
                // Events share the struct body grammar (fields only); the
                // dialect restriction and the field-type whitelist live in
                // sema so an RSC `event` gets a semantic, not a syntax, error.
                return parse_structure(declaration_kind::event,
                                       std::move(documentation));
            }
            return parse_service(std::move(documentation));
        }
        error_here(std::format("expected a declaration, found {}",
                               token_kind_name(current_.kind)));
        return std::nullopt;
    }

    [[nodiscard]] auto parse_structure(declaration_kind kind,
                                       std::string documentation)
        -> declaration {
        declaration decl;
        decl.kind = kind;
        decl.location = location();
        decl.documentation = std::move(documentation);
        advance(); // keyword
        decl.name = expect_identifier("type name").text;
        expect(token_kind::l_brace, "'{' after type name");
        decl.fields = parse_fields();
        expect(token_kind::r_brace, "'}' closing the type");
        consume(token_kind::semicolon);
        return decl;
    }

    [[nodiscard]] auto parse_packet(std::string documentation) -> declaration {
        declaration decl;
        decl.kind = declaration_kind::packet;
        decl.location = location();
        decl.documentation = std::move(documentation);
        advance(); // 'packet'
        decl.name = expect_identifier("packet name").text;
        expect(token_kind::equals, "'=' before the packet id");
        if (at(token_kind::integer)) {
            decl.packet_id = current_.integer;
            advance();
        } else {
            error_here("expected an integer packet id");
        }
        expect(token_kind::l_brace, "'{' after the packet id");
        decl.fields = parse_fields();
        expect(token_kind::r_brace, "'}' closing the packet");
        consume(token_kind::semicolon);
        return decl;
    }

    [[nodiscard]] auto parse_fields() -> std::vector<field_decl> {
        auto fields{std::vector<field_decl>{}};
        while (!at(token_kind::r_brace) && !at(token_kind::end_of_file)) {
            const auto line{current_.line};
            const auto column{current_.column};
            auto documentation{parse_documentation()};
            auto attributes{parse_attributes()};
            field_decl field;
            field.location = location();
            field.documentation = std::move(documentation);
            field.attributes = std::move(attributes);
            if (kind_ == dialect::rsc) {
                // RSC data syntax is `name: type`.
                field.name = expect_identifier("field name").text;
                expect(token_kind::colon, "':' after the RSC field name");
                field.type = parse_type();
            } else {
                // RIDL contract syntax is `type name`.
                field.type = parse_type();
                field.name = expect_identifier("field name").text;
            }
            if (consume(token_kind::equals)) {
                field.default_value = parse_value_text();
            }
            expect(token_kind::semicolon, "';' after the field");
            fields.push_back(std::move(field));
            if (current_.line == line && current_.column == column) {
                advance(); // defensive: guarantee loop progress
            }
        }
        return fields;
    }

    [[nodiscard]] auto parse_enum_like(declaration_kind kind,
                                       std::string documentation)
        -> declaration {
        declaration decl;
        decl.kind = kind;
        decl.location = location();
        decl.documentation = std::move(documentation);
        advance(); // keyword
        decl.name = expect_identifier("type name").text;
        expect(token_kind::colon, "':' before the underlying integer type");
        decl.underlying = expect_identifier("underlying integer type").text;
        if (!is_integer_type_name(decl.underlying)) {
            error_here(std::format(
                "enum/flags underlying type must be an integer type, got '{}'",
                decl.underlying));
        }
        expect(token_kind::l_brace, "'{' after the underlying type");
        while (!at(token_kind::r_brace) && !at(token_kind::end_of_file)) {
            const auto line{current_.line};
            const auto column{current_.column};
            enum_entry entry;
            entry.location = location();
            entry.documentation = parse_documentation();
            entry.name = expect_identifier("enum entry name").text;
            if (consume(token_kind::equals)) {
                if (at(token_kind::integer)) {
                    entry.value = current_.integer;
                    advance();
                } else {
                    error_here("expected an integer enum value");
                }
            }
            decl.entries.push_back(std::move(entry));
            if (!consume(token_kind::comma)) {
                break;
            }
            if (current_.line == line && current_.column == column) {
                advance();
            }
        }
        expect(token_kind::r_brace, "'}' closing the enum");
        consume(token_kind::semicolon);
        return decl;
    }

    [[nodiscard]] auto parse_handle(std::string documentation) -> declaration {
        declaration decl;
        decl.kind = declaration_kind::handle;
        decl.location = location();
        decl.documentation = std::move(documentation);
        advance(); // 'handle'
        expect(token_kind::l_angle, "'<' after 'handle'");
        decl.name = expect_identifier("handle tag type name").text;
        expect(token_kind::r_angle, "'>' closing the handle type");
        expect(token_kind::semicolon, "';' after the handle declaration");
        return decl;
    }

    [[nodiscard]] auto parse_service(std::string documentation) -> declaration {
        declaration decl;
        decl.kind = declaration_kind::service;
        decl.location = location();
        decl.documentation = std::move(documentation);
        advance(); // 'service'
        decl.name = expect_identifier("service name").text;
        expect(token_kind::colon, "':' before the service version");
        if (at(token_kind::floating) || at(token_kind::integer)) {
            decl.service.version = current_.text;
            advance();
        } else {
            error_here("expected a service version (a full SemVer triple, e.g. "
                       "1.0.0)");
        }
        expect(token_kind::l_brace, "'{' opening the service body");
        while (!at(token_kind::r_brace) && !at(token_kind::end_of_file)) {
            const auto line{current_.line};
            const auto column{current_.column};
            auto member_documentation{parse_documentation()};
            auto member_attributes{parse_attributes()};
            if (at_identifier("fn")) {
                decl.methods.push_back(
                    parse_method(std::move(member_documentation),
                                 std::move(member_attributes)));
            } else if (at(token_kind::identifier)) {
                parse_service_meta(decl);
            } else {
                error_here("expected a service method ('fn ...') or metadata");
                advance();
            }
            if (current_.line == line && current_.column == column) {
                advance();
            }
        }
        expect(token_kind::r_brace, "'}' closing the service");
        consume(token_kind::semicolon);
        return decl;
    }

    void parse_service_meta(declaration &service_declaration) {
        const auto key_location{location()};
        const auto key{current_.text};
        advance(); // key
        expect(token_kind::equals, "'=' after the service metadata key");
        if (at(token_kind::l_bracket)) {
            auto values{parse_list()};
            if (key == "provides") {
                service_declaration.service.provides = std::move(values);
            } else if (key == "requires") {
                service_declaration.service.requires_list = std::move(values);
            } else if (key == "capabilities") {
                service_declaration.service.capabilities = std::move(values);
            } else if (key == "emits") {
                service_declaration.service.emits = std::move(values);
            } else if (key == "consumes") {
                service_declaration.service.consumes = std::move(values);
            } else {
                diagnostics_.error(
                    key_location,
                    std::format("unknown list metadata '{}'", key));
            }
        } else if (at(token_kind::identifier)) {
            const auto value{current_.text};
            advance();
            if (key == "side") {
                service_declaration.service.side = value;
            } else if (key == "thread") {
                service_declaration.service.thread = value;
            } else if (key == "composition") {
                service_declaration.service.composition = value;
            } else {
                diagnostics_.error(key_location,
                                   std::format("unknown metadata '{}'", key));
            }
        } else {
            error_here("expected a metadata value or list");
        }
        expect(token_kind::semicolon, "';' after service metadata");
    }

    [[nodiscard]] auto parse_list() -> std::vector<std::string> {
        auto values{std::vector<std::string>{}};
        expect(token_kind::l_bracket, "'[' opening the list");
        while (!at(token_kind::r_bracket) && !at(token_kind::end_of_file)) {
            if (at(token_kind::string) || at(token_kind::identifier)) {
                values.push_back(current_.text);
                advance();
            } else {
                error_here("expected a string or identifier in the list");
                advance();
                break;
            }
            if (!consume(token_kind::comma)) {
                break;
            }
        }
        expect(token_kind::r_bracket, "']' closing the list");
        return values;
    }

    [[nodiscard]] auto parse_method(std::string documentation,
                                    std::vector<attribute> attributes)
        -> method_decl {
        method_decl method;
        method.documentation = std::move(documentation);
        method.attributes = std::move(attributes);
        method.location = location();
        advance(); // 'fn'
        method.name = expect_identifier("method name").text;
        expect(token_kind::l_paren, "'(' after the method name");
        while (!at(token_kind::r_paren) && !at(token_kind::end_of_file)) {
            method.parameters.push_back(parse_parameter());
            if (!consume(token_kind::comma)) {
                break;
            }
        }
        expect(token_kind::r_paren, "')' after the method parameters");
        if (consume(token_kind::arrow)) {
            method.has_return = true;
            method.return_type = parse_type();
        }
        expect(token_kind::semicolon, "';' after the method declaration");
        return method;
    }

    [[nodiscard]] auto parse_parameter() -> field_decl {
        field_decl parameter;
        parameter.attributes = parse_attributes();
        parameter.location = location();
        if (at_identifier("in")) {
            advance();
            parameter.out = false;
        } else if (at_identifier("out")) {
            advance();
            parameter.out = true;
        } else {
            // Methods use `out`/`in` explicitly in RIDL v0; an omitted
            // direction means a borrowed input parameter.
            parameter.out = false;
        }
        parameter.type = parse_type();
        parameter.name = expect_identifier("parameter name").text;
        return parameter;
    }

    [[nodiscard]] auto parse_type() -> type_ref {
        type_ref type;
        type.location = location();
        // Bounded recursion. A pathological `array<array<...>>` nesting would
        // overflow the stack (SIGSEGV, no diagnostic); stop here and let the
        // caller's recovery/synchronize path unwind the declaration.
        if (type_depth_ >= max_type_depth) {
            error_here(
                std::format("type nesting exceeds the maximum depth of {}",
                            max_type_depth));
            return type;
        }
        ++type_depth_;
        struct depth_guard {
            std::uint32_t &depth;
            ~depth_guard() { --depth; }
        };
        const depth_guard guard{type_depth_};

        type.name = expect_identifier("type name").text;
        while (consume(token_kind::colon_colon)) {
            type.name += "::";
            type.name += expect_identifier("qualified type name segment").text;
        }
        if (consume(token_kind::l_angle)) {
            for (;;) {
                type.arguments.push_back(parse_type());
                if (!consume(token_kind::comma)) {
                    break;
                }
            }
            expect(token_kind::r_angle, "'>' closing the type arguments");
        }
        return type;
    }

    [[nodiscard]] auto parse_value_text() -> std::string {
        if (at(token_kind::integer) || at(token_kind::floating) ||
            at(token_kind::string)) {
            auto text{current_.text};
            advance();
            return text;
        }
        if (at(token_kind::identifier)) {
            auto text{current_.text};
            advance();
            return text;
        }
        error_here("expected a default value");
        return {};
    }

    /// Skip a malformed declaration: consume tokens until the current
    /// declaration's closing brace (or a top-level semicolon).
    void synchronize() {
        auto depth{0};
        while (!at(token_kind::end_of_file)) {
            if (at(token_kind::l_brace)) {
                ++depth;
            } else if (at(token_kind::r_brace)) {
                --depth;
                if (depth <= 0) {
                    advance();
                    return;
                }
            } else if (at(token_kind::semicolon) && depth == 0) {
                advance();
                return;
            }
            advance();
        }
    }

    dialect kind_;
    std::string path_;
    lexer lexer_;
    diagnostics &diagnostics_;
    token current_;
    /// Current type-expression recursion depth (recursion guard).
    std::uint32_t type_depth_{0};
};

} // namespace

auto parse_schema(parse_request request, std::string_view source,
                  diagnostics &sink) -> std::optional<source_unit> {
    const auto baseline{sink.size()};
    parser_impl parser{request.kind, std::move(request.path), source, sink};
    auto unit{parser.parse()};
    if (sink.size() != baseline) {
        return std::nullopt;
    }
    return unit;
}

} // namespace refrain::schema
