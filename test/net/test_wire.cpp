// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/net/wire.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace boost::ut;

namespace net = refrain::net;

const suite tests = [] {
    "text component writes a network nbt string tag"_test = [] {
        const net::text_component value{.text = "abc"};
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_text_component(out, value)));
        expect(bytes.size() == 8U);
        expect(bytes[0] == std::byte{8});
        expect(bytes[1] == std::byte{0});
        expect(bytes[2] == std::byte{0});
        expect(bytes[3] == std::byte{0});
        expect(bytes[4] == std::byte{3});
        expect(bytes[5] == std::byte{'a'});
        expect(bytes[7] == std::byte{'c'});
    };

    "text component round trips"_test = [] {
        const net::text_component value{.text = "Hello, Refrain!"};
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_text_component(out, value)));
        net::reader in{bytes};
        net::text_component decoded{};
        expect(static_cast<bool>(net::decode_text_component(in, decoded)));
        expect(decoded == value);
        expect(in.empty());
    };

    "text component rejects a compound tag"_test = [] {
        const std::array bytes{std::byte{10}, std::byte{0}, std::byte{0}};
        net::reader in{bytes};
        net::text_component decoded{};
        const auto status{net::decode_text_component(in, decoded)};
        expect(!status);
        expect(status.error().code == refrain::error_code::unsupported);
    };

    "text component rejects nul and supplementary characters"_test = [] {
        for (const std::string_view text :
             {std::string_view{"a\0b", 3},
              std::string_view{"\xF0\x9F\x98\x80"}}) {
            std::vector<std::byte> bytes;
            net::writer out{bytes};
            const auto status{net::encode_text_component(
                out, net::text_component{std::string{text}})};
            expect(!status);
            expect(status.error().code ==
                   refrain::error_code::invalid_argument);
        }
    };

    "debug subscription updates and events round trip"_test = [] {
        net::debug_subscription_update update{};
        update.subscription = 1U;
        update.has_value = true;
        update.value.has_a = true;
        update.value.pos_a = 1234;
        update.value.u_a = 7U;
        update.value.positions_a = {42, 43};
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(
            net::encode_debug_subscription_update(out, update)));
        net::reader in{bytes};
        net::debug_subscription_update decoded{};
        expect(static_cast<bool>(
            net::decode_debug_subscription_update(in, decoded)));
        expect(decoded.subscription == 1U);
        expect(decoded.has_value);
        expect(decoded.value.has_a);
        expect(decoded.value.pos_a == 1234);
        expect(decoded.value.u_a == 7U);
        expect(decoded.value.positions_a.size() == 2U);
        expect(in.empty());

        net::debug_subscription_update empty{};
        empty.subscription = 14U;
        empty.has_value = false;
        bytes.clear();
        net::writer empty_out{bytes};
        expect(static_cast<bool>(
            net::encode_debug_subscription_update(empty_out, empty)));
        net::reader empty_in{bytes};
        net::debug_subscription_update empty_decoded{};
        expect(static_cast<bool>(
            net::decode_debug_subscription_update(empty_in, empty_decoded)));
        expect(!empty_decoded.has_value);
        expect(empty_in.empty());

        net::debug_subscription_event event{};
        event.subscription = 15U;
        event.value.u_a = 3U;
        event.value.d_a = 1.0;
        event.value.d_b = 2.0;
        event.value.d_c = 3.0;
        bytes.clear();
        net::writer event_out{bytes};
        expect(static_cast<bool>(
            net::encode_debug_subscription_event(event_out, event)));
        net::reader event_in{bytes};
        net::debug_subscription_event event_decoded{};
        expect(static_cast<bool>(
            net::decode_debug_subscription_event(event_in, event_decoded)));
        expect(event_decoded.subscription == 15U);
        expect(event_decoded.value.u_a == 3U);
        expect(event_decoded.value.d_c == 3.0);
        expect(event_in.empty());
    };

    "particle options round trip simple and parameterized types"_test = [] {
        net::particle_options simple{};
        simple.type = 0U;
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_particle_options(out, simple)));
        expect(bytes.size() == 1U);
        net::reader in{bytes};
        net::particle_options decoded{};
        expect(static_cast<bool>(net::decode_particle_options(in, decoded)));
        expect(decoded.type == 0U);
        expect(in.empty());

        net::particle_options dust{};
        dust.type = 21U;
        dust.color = 0x112233;
        dust.scale = 1.5F;
        bytes.clear();
        net::writer dust_out{bytes};
        expect(static_cast<bool>(net::encode_particle_options(dust_out, dust)));
        net::reader dust_in{bytes};
        net::particle_options dust_decoded{};
        expect(static_cast<bool>(
            net::decode_particle_options(dust_in, dust_decoded)));
        expect(dust_decoded.type == 21U);
        expect(dust_decoded.color == 0x112233);
        expect(dust_decoded.scale == 1.5F);
        expect(dust_in.empty());

        net::particle_options block{};
        block.type = 1U;
        block.block_state = 42U;
        bytes.clear();
        net::writer block_out{bytes};
        expect(
            static_cast<bool>(net::encode_particle_options(block_out, block)));
        net::reader block_in{bytes};
        net::particle_options block_decoded{};
        expect(static_cast<bool>(
            net::decode_particle_options(block_in, block_decoded)));
        expect(block_decoded.block_state == 42U);

        net::particle_options item{};
        item.type = 57U;
        item.stack = {.item_id = 5U, .count = 2U};
        bytes.clear();
        net::writer item_out{bytes};
        expect(static_cast<bool>(net::encode_particle_options(item_out, item)));
        net::reader item_in{bytes};
        net::particle_options item_decoded{};
        expect(static_cast<bool>(
            net::decode_particle_options(item_in, item_decoded)));
        expect(item_decoded.stack.item_id == 5U);

        net::particle_options vibration{};
        vibration.type = 58U;
        vibration.entity_source = true;
        vibration.entity_id = 12;
        vibration.y_offset = 0.5F;
        vibration.arrival_ticks = 20;
        bytes.clear();
        net::writer vibration_out{bytes};
        expect(static_cast<bool>(
            net::encode_particle_options(vibration_out, vibration)));
        net::reader vibration_in{bytes};
        net::particle_options vibration_decoded{};
        expect(static_cast<bool>(
            net::decode_particle_options(vibration_in, vibration_decoded)));
        expect(vibration_decoded.entity_source);
        expect(vibration_decoded.entity_id == 12);
        expect(vibration_decoded.arrival_ticks == 20);

        net::particle_options unknown{};
        unknown.type = 200U;
        bytes.clear();
        net::writer unknown_out{bytes};
        const auto status{net::encode_particle_options(unknown_out, unknown)};
        expect(!status);
        expect(status.error().code == refrain::error_code::invalid_argument);
    };

    "recipe slots and displays round trip"_test = [] {
        const auto tag{
            refrain::data::resource_location::parse("minecraft:planks")};
        expect(tag.has_value());
        if (!tag) {
            return;
        }
        net::item_holder_set named{};
        named.named = true;
        named.tag = *tag;
        net::item_holder_set direct{};
        direct.ids = {4U, 5U};
        for (const auto &set : {named, direct}) {
            std::vector<std::byte> bytes;
            net::writer out{bytes};
            expect(static_cast<bool>(net::encode_holder_set(out, set)));
            net::reader in{bytes};
            net::item_holder_set decoded{};
            expect(static_cast<bool>(net::decode_holder_set(in, decoded)));
            const bool same{decoded == set};
            expect(same);
            expect(in.empty());
        }

        net::slot_display item{};
        item.type = 4U;
        item.item_id = 7U;
        net::slot_display composite{};
        composite.type = 10U;
        composite.children = {item, item};
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_slot_display(out, composite)));
        net::reader in{bytes};
        net::slot_display decoded{};
        expect(static_cast<bool>(net::decode_slot_display(in, decoded)));
        expect(decoded.type == 10U);
        expect(decoded.children.size() == 2U);
        if (decoded.children.size() == 2U) {
            expect(decoded.children[0].type == 4U);
            expect(decoded.children[0].item_id == 7U);
        }
        expect(in.empty());

        net::recipe_display display{};
        display.type = 0U;
        display.slots = {item, item};
        display.result = item;
        display.station = item;
        bytes.clear();
        net::writer display_out{bytes};
        expect(static_cast<bool>(
            net::encode_recipe_display(display_out, display)));
        net::reader display_in{bytes};
        net::recipe_display display_decoded{};
        expect(static_cast<bool>(
            net::decode_recipe_display(display_in, display_decoded)));
        expect(display_decoded.type == 0U);
        expect(display_decoded.slots.size() == 2U);
        expect(display_decoded.result.item_id == 7U);
        expect(display_in.empty());

        net::recipe_display_entry entry{};
        entry.id = 9U;
        entry.display = display;
        entry.group = 3U;
        entry.category = 1U;
        entry.has_requirements = true;
        entry.requirements = {direct};
        bytes.clear();
        net::writer entry_out{bytes};
        expect(static_cast<bool>(
            net::encode_recipe_display_entry(entry_out, entry)));
        net::reader entry_in{bytes};
        net::recipe_display_entry entry_decoded{};
        expect(static_cast<bool>(
            net::decode_recipe_display_entry(entry_in, entry_decoded)));
        expect(entry_decoded.id == 9U);
        expect(entry_decoded.group == 3U);
        expect(entry_decoded.requirements.size() == 1U);
        expect(entry_in.empty());
    };

    "advancement round trips display, requirements and progress"_test = [] {
        const auto background{refrain::data::resource_location::parse(
            "minecraft:textures/block/stone.png")};
        expect(background.has_value());
        if (!background) {
            return;
        }
        net::advancement value{};
        value.parent.reset();
        value.has_display = true;
        value.display.title.text = "Title";
        value.display.description.text = "Description";
        value.display.icon = {.item_id = 5U, .count = 1U};
        value.display.frame = 1U;
        value.display.flags = 1;
        value.display.background = *background;
        value.requirements.groups = {{"criterion_a"},
                                     {"criterion_b", "criterion_c"}};
        value.sends_telemetry = true;
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_advancement(out, value)));
        net::reader in{bytes};
        net::advancement decoded{};
        expect(static_cast<bool>(net::decode_advancement(in, decoded)));
        expect(decoded.has_display);
        expect(decoded.display.title.text == "Title");
        expect(decoded.display.background.has_value());
        expect(decoded.requirements.groups.size() == 2U);
        expect(decoded.sends_telemetry);
        expect(in.empty());

        net::advancement_progress progress{};
        progress.criteria.push_back(
            {.criterion = "a", .obtained = std::int64_t{42}});
        progress.criteria.push_back(
            {.criterion = "b", .obtained = std::nullopt});
        bytes.clear();
        net::writer progress_out{bytes};
        expect(static_cast<bool>(
            net::encode_advancement_progress(progress_out, progress)));
        net::reader progress_in{bytes};
        net::advancement_progress progress_decoded{};
        expect(static_cast<bool>(
            net::decode_advancement_progress(progress_in, progress_decoded)));
        expect(progress_decoded.criteria.size() == 2U);
        if (progress_decoded.criteria.size() == 2U) {
            expect(progress_decoded.criteria[0].obtained == std::int64_t{42});
            expect(!progress_decoded.criteria[1].obtained.has_value());
        }
        expect(progress_in.empty());
    };

    "entity metadata round trips common serializers"_test = [] {
        net::entity_metadata metadata{};
        net::metadata_value byte_value{};
        byte_value.index = 0U;
        byte_value.serializer = 0U;
        byte_value.integer = -5;
        metadata.values.push_back(byte_value);
        net::metadata_value int_value{};
        int_value.index = 1U;
        int_value.serializer = 1U;
        int_value.integer = 300;
        metadata.values.push_back(int_value);
        net::metadata_value text_value{};
        text_value.index = 2U;
        text_value.serializer = 4U;
        text_value.text = "hello";
        metadata.values.push_back(text_value);
        net::metadata_value component_value{};
        component_value.index = 3U;
        component_value.serializer = 5U;
        component_value.component.text = "Title";
        metadata.values.push_back(component_value);
        net::metadata_value stack_value{};
        stack_value.index = 4U;
        stack_value.serializer = 7U;
        stack_value.stack = net::item_stack{.count = 2U, .item_id = 5U};
        metadata.values.push_back(stack_value);
        net::metadata_value pos_value{};
        pos_value.index = 5U;
        pos_value.serializer = 10U;
        pos_value.integer = 1234;
        metadata.values.push_back(pos_value);
        net::metadata_value villager{};
        villager.index = 6U;
        villager.serializer = 18U;
        villager.ids = {1U, 2U, 3U};
        metadata.values.push_back(villager);
        net::metadata_value vector{};
        vector.index = 7U;
        vector.serializer = 39U;
        vector.vector = {1.0, 2.0, 3.0, 0.0};
        metadata.values.push_back(vector);

        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_entity_metadata(out, metadata)));
        net::reader in{bytes};
        net::entity_metadata decoded{};
        expect(static_cast<bool>(net::decode_entity_metadata(in, decoded)));
        expect(decoded.values.size() == metadata.values.size());
        if (decoded.values.size() == metadata.values.size()) {
            expect(decoded.values[0].integer == -5);
            expect(decoded.values[1].integer == 300);
            expect(decoded.values[2].text == "hello");
            expect(decoded.values[3].component.text == "Title");
            expect(decoded.values[4].stack.count == 2U);
            expect(decoded.values[5].integer == 1234);
            expect(decoded.values[6].ids[0] == 1U &&
                   decoded.values[6].ids[2] == 3U);
            expect(decoded.values[7].vector[2] == 3.0);
        }
        expect(in.empty());

        const std::array unsupported{std::byte{0}, std::byte{16}};
        net::reader unsupported_in{unsupported};
        net::entity_metadata rejected{};
        const auto status{
            net::decode_entity_metadata(unsupported_in, rejected)};
        expect(!status);
        expect(status.error().code == refrain::error_code::unsupported);
    };

    "player info update round trips action-gated fields"_test = [] {
        net::player_info_update value{};
        value.actions =
            static_cast<std::uint8_t>(1U | 4U | 8U | 16U | 32U | 64U | 128U);
        net::player_info_entry entry{};
        const auto id{net::uuid::parse("01234567-89ab-cdef-0123-456789abcdef")};
        expect(id.has_value());
        if (!id) {
            return;
        }
        entry.id = *id;
        entry.name = "TestPlayer";
        entry.properties.push_back(
            net::profile_property{.name = "textures",
                                  .value = "dGVzdA==",
                                  .signature = std::string{"c2ln"}});
        entry.game_mode = 1U;
        entry.listed = true;
        entry.latency = 42U;
        entry.has_display_name = true;
        entry.display_name.text = "Test";
        entry.list_order = 3U;
        entry.show_hat = true;
        value.entries.push_back(entry);
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_player_info_update(out, value)));
        net::reader in{bytes};
        net::player_info_update decoded{};
        expect(static_cast<bool>(net::decode_player_info_update(in, decoded)));
        expect(decoded.actions == value.actions);
        expect(decoded.entries.size() == 1U);
        if (decoded.entries.size() == 1U) {
            const auto &actual{decoded.entries.front()};
            expect(actual.name == "TestPlayer");
            expect(actual.properties.size() == 1U);
            if (actual.properties.size() == 1U) {
                expect(actual.properties.front().name == "textures");
                expect(actual.properties.front().signature.has_value());
            }
            expect(actual.game_mode == 1U);
            expect(actual.listed == true);
            expect(actual.latency == 42U);
            expect(actual.has_display_name);
            expect(actual.display_name.text == "Test");
            expect(actual.list_order == 3U);
            expect(actual.show_hat == true);
        }
        expect(in.empty());
    };

    "tracked waypoint round trips block position and empty forms"_test = [] {
        const auto style{
            refrain::data::resource_location::parse("minecraft:default")};
        expect(style.has_value());
        if (!style) {
            return;
        }
        net::tracked_waypoint point{};
        point.operation = 0U;
        point.has_uuid = true;
        const auto id{net::uuid::parse("01234567-89ab-cdef-0123-456789abcdef")};
        expect(id.has_value());
        if (!id) {
            return;
        }
        point.id = *id;
        point.style = *style;
        point.color = 0x112233U;
        point.type = 1U;
        point.x = 10;
        point.y = 64;
        point.z = -20;
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_tracked_waypoint(out, point)));
        net::reader in{bytes};
        net::tracked_waypoint decoded{};
        expect(static_cast<bool>(net::decode_tracked_waypoint(in, decoded)));
        expect(decoded.operation == 0U);
        expect(decoded.type == 1U);
        expect(decoded.x == 10 && decoded.y == 64 && decoded.z == -20);
        expect(decoded.color == 0x112233U);
        expect(decoded.style.to_string() == style->to_string());
        expect(in.empty());

        net::tracked_waypoint empty{};
        empty.operation = 1U;
        empty.has_uuid = false;
        empty.name = "marker";
        empty.style = *style;
        empty.type = 0U;
        bytes.clear();
        net::writer empty_out{bytes};
        expect(
            static_cast<bool>(net::encode_tracked_waypoint(empty_out, empty)));
        net::reader empty_in{bytes};
        net::tracked_waypoint empty_decoded{};
        expect(static_cast<bool>(
            net::decode_tracked_waypoint(empty_in, empty_decoded)));
        expect(empty_decoded.has_uuid == false);
        expect(empty_decoded.name == "marker");
        expect(empty_decoded.type == 0U);
        expect(empty_in.empty());
    };

    "sound selector round trips flags, source and name"_test = [] {
        const auto name{refrain::data::resource_location::parse(
            "minecraft:block.note_block")};
        expect(name.has_value());
        if (!name) {
            return;
        }
        const net::sound_selector value{
            .flags = 3U, .source = 4U, .name = *name};
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_sound_selector(out, value)));
        net::reader in{bytes};
        net::sound_selector decoded{};
        expect(static_cast<bool>(net::decode_sound_selector(in, decoded)));
        expect(decoded.flags == 3U);
        expect(decoded.source == 4U);
        expect(decoded.name.to_string() == name->to_string());
        expect(in.empty());

        const net::sound_selector none{};
        bytes.clear();
        net::writer none_out{bytes};
        expect(static_cast<bool>(net::encode_sound_selector(none_out, none)));
        expect(bytes.size() == 1U);
    };

    "objective update round trips add and remove forms"_test = [] {
        net::objective_update add{};
        add.name = "sidebar";
        add.method = 0U;
        add.display.text = "Scoreboard";
        add.render_type = 1U;
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_objective_update(out, add)));
        net::reader in{bytes};
        net::objective_update decoded{};
        expect(static_cast<bool>(net::decode_objective_update(in, decoded)));
        expect(decoded.name == add.name);
        expect(decoded.method == 0U);
        expect(decoded.display.text == add.display.text);
        expect(decoded.render_type == 1U);
        expect(in.empty());

        net::objective_update remove{};
        remove.name = "sidebar";
        remove.method = 1U;
        bytes.clear();
        net::writer remove_out{bytes};
        expect(static_cast<bool>(
            net::encode_objective_update(remove_out, remove)));
        net::reader remove_in{bytes};
        net::objective_update remove_decoded{};
        expect(static_cast<bool>(
            net::decode_objective_update(remove_in, remove_decoded)));
        expect(remove_decoded.method == 1U);
        expect(remove_in.empty());
    };

    "team update round trips create and leave forms"_test = [] {
        net::team_update create{};
        create.name = "red";
        create.method = 0U;
        net::team_parameters parameters{};
        parameters.display.text = "Red Team";
        parameters.prefix.text = "[R] ";
        parameters.suffix.text = "";
        parameters.visibility = 0U;
        parameters.collision = 0U;
        parameters.color = 1U;
        parameters.options = 0x03U;
        create.parameters = parameters;
        create.players = {"alice", "bob"};
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_team_update(out, create)));
        net::reader in{bytes};
        net::team_update decoded{};
        expect(static_cast<bool>(net::decode_team_update(in, decoded)));
        expect(decoded.name == create.name);
        expect(decoded.method == 0U);
        expect(decoded.parameters.has_value());
        if (decoded.parameters) {
            expect(decoded.parameters->display.text == "Red Team");
            expect(decoded.parameters->color == 1U);
            expect(decoded.parameters->options == 0x03U);
        }
        const bool same_players{decoded.players == create.players};
        expect(same_players);
        expect(in.empty());

        net::team_update leave{};
        leave.name = "red";
        leave.method = 4U;
        leave.players = {"alice"};
        bytes.clear();
        net::writer leave_out{bytes};
        expect(static_cast<bool>(net::encode_team_update(leave_out, leave)));
        net::reader leave_in{bytes};
        net::team_update leave_decoded{};
        expect(static_cast<bool>(
            net::decode_team_update(leave_in, leave_decoded)));
        expect(leave_decoded.method == 4U);
        expect(!leave_decoded.parameters.has_value());
        const bool same_leave{leave_decoded.players == leave.players};
        expect(same_leave);
        expect(leave_in.empty());
    };

    "movement delta round trips linear and stepped forms"_test = [] {
        const net::vec_delta linear{.on_ground = true, .x = 1, .y = -2, .z = 3};
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_vec_delta(out, linear)));
        expect(bytes.size() == 7U);
        net::reader in{bytes};
        net::vec_delta decoded{};
        expect(static_cast<bool>(net::decode_vec_delta(in, decoded)));
        expect(decoded.on_ground);
        expect(decoded.x == 1 && decoded.y == -2 && decoded.z == 3);
        expect(decoded.steps.empty());
        expect(in.empty());

        net::vec_delta stepped{};
        stepped.on_ground = false;
        stepped.steps = {{5U, 1, 2, 3}, {6U, -1, -2, -3}};
        bytes.clear();
        net::writer stepped_out{bytes};
        expect(static_cast<bool>(net::encode_vec_delta(stepped_out, stepped)));
        net::reader stepped_in{bytes};
        net::vec_delta stepped_decoded{};
        expect(static_cast<bool>(
            net::decode_vec_delta(stepped_in, stepped_decoded)));
        const bool same{stepped_decoded.steps == stepped.steps};
        expect(same);
        expect(stepped_in.empty());
    };

    "advancement screen selection round trips both actions"_test = [] {
        const auto tab{
            refrain::data::resource_location::parse("minecraft:story/root")};
        expect(tab.has_value());
        if (!tab) {
            return;
        }
        const net::advancement_seen opened{.action = 0U, .tab = *tab};
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_advancement_seen(out, opened)));
        net::reader in{bytes};
        net::advancement_seen decoded{};
        expect(static_cast<bool>(net::decode_advancement_seen(in, decoded)));
        expect(decoded.action == 0U);
        expect(decoded.tab.to_string() == tab->to_string());
        expect(in.empty());

        const net::advancement_seen closed{.action = 1U};
        bytes.clear();
        net::writer closed_out{bytes};
        expect(static_cast<bool>(
            net::encode_advancement_seen(closed_out, closed)));
        expect(bytes.size() == 1U);
        net::reader closed_in{bytes};
        net::advancement_seen closed_decoded{.action = 0U};
        expect(static_cast<bool>(
            net::decode_advancement_seen(closed_in, closed_decoded)));
        expect(closed_decoded.action == 1U);
    };

    "stat entry round trips two varints"_test = [] {
        const net::stat_entry value{.type = 3U, .value = 42U};
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_stat_entry(out, value)));
        expect(bytes.size() == 2U);
        net::reader in{bytes};
        net::stat_entry decoded{};
        expect(static_cast<bool>(net::decode_stat_entry(in, decoded)));
        const bool same{decoded == value};
        expect(same);
        expect(in.empty());
    };

    "position path round trips linear and stepped forms"_test = [] {
        const net::position_path linear{
            .type = 0U, .end = {1.5, -2.0, 3.25}, .steps = {}};
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_position_path(out, linear)));
        net::reader in{bytes};
        net::position_path decoded{};
        expect(static_cast<bool>(net::decode_position_path(in, decoded)));
        expect(decoded.type == 0U);
        expect(decoded.end == linear.end);
        expect(in.empty());

        net::position_path stepped{};
        stepped.type = 1U;
        stepped.steps = {{1.0, 2.0, 3.0, 5U}, {4.0, 5.0, 6.0, 7U}};
        bytes.clear();
        net::writer stepped_out{bytes};
        expect(
            static_cast<bool>(net::encode_position_path(stepped_out, stepped)));
        net::reader stepped_in{bytes};
        net::position_path stepped_decoded{};
        expect(static_cast<bool>(
            net::decode_position_path(stepped_in, stepped_decoded)));
        const bool same_steps{stepped_decoded.steps == stepped.steps};
        expect(same_steps);
        expect(stepped_in.empty());

        net::position_path invalid{};
        invalid.type = 5U;
        std::vector<std::byte> sink;
        net::writer invalid_out{sink};
        const auto status{net::encode_position_path(invalid_out, invalid)};
        expect(!status);
        expect(status.error().code == refrain::error_code::invalid_argument);
    };

    "map patch round trips empty and present forms"_test = [] {
        net::map_patch empty{};
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_map_patch(out, empty)));
        expect(bytes.size() == 1U);
        net::reader in{bytes};
        net::map_patch decoded{};
        expect(static_cast<bool>(net::decode_map_patch(in, decoded)));
        expect(decoded.width == 0U);
        expect(in.empty());

        net::map_patch patch{
            .width = 2U,
            .height = 3U,
            .start_x = 4U,
            .start_y = 5U,
            .colors = {std::byte{1}, std::byte{2}, std::byte{3}}};
        bytes.clear();
        net::writer patch_out{bytes};
        expect(static_cast<bool>(net::encode_map_patch(patch_out, patch)));
        net::reader patch_in{bytes};
        net::map_patch patch_decoded{};
        expect(
            static_cast<bool>(net::decode_map_patch(patch_in, patch_decoded)));
        const bool same_patch{patch_decoded == patch};
        expect(same_patch);
        expect(patch_in.empty());
    };

    "filter mask round trips both short and partial forms"_test = [] {
        for (const std::uint32_t type : {0U, 1U}) {
            net::filter_mask value{.type = type};
            std::vector<std::byte> bytes;
            net::writer out{bytes};
            expect(static_cast<bool>(net::encode_filter_mask(out, value)));
            expect(bytes.size() == 1U);
            net::reader in{bytes};
            net::filter_mask decoded{.type = 9U};
            expect(static_cast<bool>(net::decode_filter_mask(in, decoded)));
            expect(decoded.type == type);
            expect(decoded.mask.empty());
            expect(in.empty());
        }
        net::filter_mask partial{.type = 2U,
                                 .mask = {std::byte{0xAA}, std::byte{0x01}}};
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_filter_mask(out, partial)));
        net::reader in{bytes};
        net::filter_mask decoded{};
        expect(static_cast<bool>(net::decode_filter_mask(in, decoded)));
        expect(decoded.type == 2U);
        const bool same{decoded.mask == partial.mask};
        expect(same);
        expect(in.empty());
    };

    "packed message signature round trips both forms"_test = [] {
        net::message_signature_packed id_form{};
        id_form.id = 5U;
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(
            net::encode_message_signature_packed(out, id_form)));
        net::reader in{bytes};
        net::message_signature_packed decoded{};
        expect(static_cast<bool>(
            net::decode_message_signature_packed(in, decoded)));
        expect(!decoded.full);
        expect(decoded.id == 5U);
        expect(in.empty());

        net::message_signature_packed full_form{};
        full_form.full = true;
        for (std::size_t index{0}; index < full_form.signature.size();
             ++index) {
            full_form.signature[index] = static_cast<std::byte>(index & 0xFFU);
        }
        bytes.clear();
        net::writer full_out{bytes};
        expect(static_cast<bool>(
            net::encode_message_signature_packed(full_out, full_form)));
        expect(bytes.size() == 257U);
        net::reader full_in{bytes};
        net::message_signature_packed full_decoded{};
        expect(static_cast<bool>(
            net::decode_message_signature_packed(full_in, full_decoded)));
        expect(full_decoded.full);
        const bool same{full_decoded.signature == full_form.signature};
        expect(same);
        expect(full_in.empty());
    };

    "nbt end tag round trips as an empty value"_test = [] {
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_nbt(out, net::nbt_blob{})));
        const bool end_tag{bytes == std::vector<std::byte>{std::byte{0}}};
        expect(end_tag);
        net::reader in{bytes};
        net::nbt_blob decoded{};
        expect(static_cast<bool>(net::decode_nbt(in, decoded)));
        expect(decoded.bytes.empty());
        expect(in.empty());
    };

    "nbt compound round trips byte for byte"_test = [] {
        // TAG_Compound with an empty name holding one TAG_Int named "value".
        std::vector<std::byte> tag;
        const auto push = [&tag](std::initializer_list<int> values) {
            for (const int value : values) {
                tag.push_back(static_cast<std::byte>(value));
            }
        };
        push({10, 0, 0, 3, 0, 5});
        for (const char character : std::string_view{"value"}) {
            tag.push_back(static_cast<std::byte>(character));
        }
        push({0, 0, 0, 42, 0});
        const net::nbt_blob value{.bytes = tag};
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_nbt(out, value)));
        net::reader in{bytes};
        net::nbt_blob decoded{};
        expect(static_cast<bool>(net::decode_nbt(in, decoded)));
        const bool same{decoded.bytes == tag};
        expect(same);
        expect(in.empty());
    };

    "nbt rejects a truncated tag"_test = [] {
        const std::array bytes{std::byte{10}, std::byte{0}, std::byte{0},
                               std::byte{3}};
        net::reader in{bytes};
        net::nbt_blob decoded{};
        const auto status{net::decode_nbt(in, decoded)};
        expect(!status);
        expect(status.error().code == refrain::error_code::parse_error);
    };

    "lp vec3 encodes the zero vector as one byte"_test = [] {
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_lp_vec3(out, net::lp_vec3{})));
        expect(bytes.size() == 1U);
        if (bytes.size() == 1U) {
            expect(bytes[0] == std::byte{0});
        }
        net::reader in{bytes};
        net::lp_vec3 decoded{.x = 1.0};
        expect(static_cast<bool>(net::decode_lp_vec3(in, decoded)));
        expect(decoded == net::lp_vec3{});
        expect(in.empty());
    };

    "lp vec3 round trips within its quantization step"_test = [] {
        const net::lp_vec3 value{.x = 0.5, .y = -0.25, .z = 0.125};
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_lp_vec3(out, value)));
        net::reader in{bytes};
        net::lp_vec3 decoded{};
        expect(static_cast<bool>(net::decode_lp_vec3(in, decoded)));
        expect(in.empty());
        const auto close = [](double a, double b) {
            return std::abs(a - b) <= 0.01;
        };
        expect(close(decoded.x, value.x));
        expect(close(decoded.y, value.y));
        expect(close(decoded.z, value.z));
    };

    "lp vec3 rejects truncated input"_test = [] {
        const std::array<std::byte, 2> bytes{std::byte{1}, std::byte{2}};
        net::reader in{bytes};
        net::lp_vec3 decoded{};
        const auto status{net::decode_lp_vec3(in, decoded)};
        expect(!status);
        expect(status.error().code == refrain::error_code::parse_error);
    };

    "empty item stack round trips"_test = [] {
        const net::item_stack value{};
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_item_stack(out, value)));
        expect(bytes.size() == 1U);
        expect(bytes[0] == std::byte{0});
        net::reader in{bytes};
        net::item_stack decoded{.count = 7};
        expect(static_cast<bool>(net::decode_item_stack(in, decoded)));
        expect(decoded == value);
        expect(in.empty());
    };

    "item stacks round trip without component patches"_test = [] {
        const net::item_stack value{.count = 1U, .item_id = 5U};
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_item_stack(out, value)));
        expect(bytes.size() == 3U);
        net::reader in{bytes};
        net::item_stack decoded{};
        expect(static_cast<bool>(net::decode_item_stack(in, decoded)));
        const bool same{decoded == value};
        expect(same);
        expect(in.empty());

        const std::array encoded{std::byte{1}, std::byte{5}, std::byte{1}};
        net::reader patch_in{encoded};
        net::item_stack patched{};
        const auto status{net::decode_item_stack(patch_in, patched)};
        expect(!status);
        expect(status.error().code == refrain::error_code::unsupported);
    };

    "merchant offer round trips costs and result"_test = [] {
        net::merchant_offer offer{};
        offer.cost_a = {.item_id = 5U, .count = 2U};
        offer.result = {.count = 1U, .item_id = 7U};
        offer.cost_b = net::item_cost{.item_id = 9U, .count = 1U};
        offer.out_of_stock = true;
        offer.uses = 3;
        offer.max_uses = 12;
        offer.xp = 5;
        offer.special_price_diff = -2;
        offer.price_multiplier = 0.05F;
        offer.demand = 4;
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        expect(static_cast<bool>(net::encode_merchant_offer(out, offer)));
        net::reader in{bytes};
        net::merchant_offer decoded{};
        expect(static_cast<bool>(net::decode_merchant_offer(in, decoded)));
        expect(decoded.cost_a.item_id == 5U && decoded.cost_a.count == 2U);
        expect(decoded.result.count == 1U && decoded.result.item_id == 7U);
        expect(decoded.cost_b.has_value());
        if (decoded.cost_b) {
            expect(decoded.cost_b->item_id == 9U);
        }
        expect(decoded.out_of_stock);
        expect(decoded.uses == 3 && decoded.max_uses == 12);
        expect(decoded.special_price_diff == -2);
        expect(decoded.demand == 4);
        expect(in.empty());
    };

    "text component rejects invalid utf8"_test = [] {
        std::vector<std::byte> bytes;
        net::writer out{bytes};
        const auto status{net::encode_text_component(
            out, net::text_component{std::string{"\xC0\x80", 2}})};
        expect(!status);
        expect(status.error().code == refrain::error_code::invalid_argument);
    };
};

} // namespace
