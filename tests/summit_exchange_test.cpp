// Copyright 2026 Roman Altenburg (Digby)
// Licensed under the Apache License, Version 2.0.
// See LICENSE for the license text.

#include "ysfx.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using Bytes = std::vector<uint8_t>;

static void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

class Effect {
public:
    ysfx_config_u config{ysfx_config_new()};
    ysfx_u effect{ysfx_new(config.get())};
    std::vector<Bytes> output;

    explicit Effect(const char *path)
    {
        require(ysfx_load_file(effect.get(), path, 0), "load failed");
        require(ysfx_compile(effect.get(), 0), "compile failed");
        ysfx_set_sample_rate(effect.get(), 48000);
        ysfx_set_block_size(effect.get(), 256);
        ysfx_init(effect.get());
    }

    double &value(const char *name)
    {
        double *result = ysfx_find_var(effect.get(), name);
        require(result != nullptr, "missing variable");
        return *result;
    }

    void block()
    {
        ysfx_process_float(effect.get(), nullptr, nullptr, 0, 0, 256);
        ysfx_midi_event_t event{};
        while (ysfx_receive_midi(effect.get(), &event))
            output.emplace_back(event.data, event.data + event.size);
    }

    void send(const Bytes &bytes)
    {
        ysfx_midi_event_t event{0, 0, static_cast<uint32_t>(bytes.size()), bytes.data()};
        require(ysfx_send_midi(effect.get(), &event), "enqueue failed");
        block();
    }

    void startup()
    {
        for (int index = 0; index < 100; ++index) block();
        require(output == std::vector<Bytes>({
            Bytes{240, 126, 127, 6, 1, 247},
            Bytes{240, 0, 32, 41, 1, 17, 1, 51, 68, 0, 0, 0, 0, 0, 247}}),
            "startup must send identity and settings requests");
        output.clear();
    }

    void state(int state)
    {
        value("desired_state") = state;
        value("write_state") = 1;
        block();
    }

    std::vector<double> bytes(int offset, int length)
    {
        std::vector<double> result(length);
        ysfx_read_vmem(effect.get(), offset, result.data(), result.size());
        return result;
    }
};

static Bytes snapshot(bool multi, int subtype)
{
    Bytes result(multi ? 1039 : 527);
    const Bytes header{240, 0, 32, 41, 1, 17, 1, 51};
    std::copy(header.begin(), header.end(), result.begin());
    result[8] = multi ? 2 : 0;
    const std::string name = multi ? "TEST MULTI      " : "TEST SINGLE     ";
    std::copy(name.begin(), name.end(), result.begin() + (multi ? 411 : 16));
    if (multi) result[429] = subtype;
    result.back() = 247;
    return result;
}

static Bytes settings(int global, int part_a, int part_b)
{
    Bytes result(527);
    const Bytes header{240, 0, 32, 41, 1, 17, 1, 51};
    std::copy(header.begin(), header.end(), result.begin());
    result[8] = 4;
    result[31] = global - 1;
    result[57] = part_a - 1;
    result[58] = part_b - 1;
    result.back() = 247;
    return result;
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    try {
        Effect bridge(argv[2]);
        bridge.value("heartbeat") = 1;
        bridge.state(0);
        {
            Effect checker(argv[1]);
            checker.startup();
            checker.send(settings(1, 2, 3));
            require(checker.value("settings_known") == 1
                && checker.value("global_channel") == 1
                && checker.value("part_a_channel") == 2
                && checker.value("part_b_channel") == 3,
                "default channel settings not decoded");
            require(checker.value("connection_state") == 2,
                "valid settings reply did not mark Summit online");
            checker.value("identity_deadline") = -1;
            checker.block();
            require(checker.value("connection_state") == 2,
                "identity timeout overrode settings reply");
            require(checker.value("snapshot_length") == 0, "settings dump was accepted as a patch");
            checker.send(settings(5, 7, 9));
            require(checker.value("global_channel") == 5
                && checker.value("part_a_channel") == 7
                && checker.value("part_b_channel") == 9,
                "custom channel settings not decoded");
            checker.send(settings(17, 7, 9));
            require(checker.value("global_channel") == 5, "invalid channel setting accepted");
            checker.output.clear();
            const std::vector<Bytes> multi_mode_cc{Bytes{0xb0, 0x63, 0x3e}, Bytes{0xb0, 0x62, 0}, Bytes{0xb0, 0x06, 1}};
            for (const Bytes &message : multi_mode_cc) checker.send(message);
            require(checker.value("grab_mode") == 1, "incoming Multi mode NRPN was not followed");
            require(checker.output == multi_mode_cc, "incoming mode NRPN was altered or echoed");
            checker.output.clear();
            const std::vector<Bytes> single_mode_cc{Bytes{0xb0, 0x63, 0x3e}, Bytes{0xb0, 0x62, 0}, Bytes{0xb0, 0x06, 0}};
            for (const Bytes &message : single_mode_cc) checker.send(message);
            require(checker.value("grab_mode") == 0, "incoming Single mode NRPN was not followed");
            require(checker.output == single_mode_cc, "incoming mode NRPN was altered or echoed");
            checker.output.clear();
            checker.send({0xb0, 0x3e, 0});
            require(checker.value("grab_mode") == 1, "observed CC 62 value 0 did not select Multi");
            checker.send({0xb0, 0x3e, 1});
            require(checker.value("grab_mode") == 0, "observed CC 62 value 1 did not select Single");
            require(checker.output == std::vector<Bytes>({Bytes{0xb0, 0x3e, 0}, Bytes{0xb0, 0x3e, 1}}),
                "single-CC mode notifications were altered or echoed");
            checker.output.clear();
            std::vector<uint8_t> pixels(619 * 396 * 4);
            int selection = 2;
            ysfx_gfx_config_t graphics{};
            graphics.pixel_width = 619;
            graphics.pixel_height = 396;
            graphics.pixels = pixels.data();
            graphics.scale_factor = 1;
            graphics.user_data = &selection;
            graphics.show_menu = [](void *user_data, const char *, int32_t, int32_t) -> int32_t {
                return *static_cast<int *>(user_data);
            };
            ysfx_gfx_setup(checker.effect.get(), &graphics);
            auto check_colors = [&](const std::vector<uint32_t> &colors) {
                for (int index = 0; index < 4; ++index) {
                    size_t pixel = (175 * 619 + 20 + 150 * index) * 4;
                    uint32_t color = (pixels[pixel + 2] << 16) | (pixels[pixel + 1] << 8) | pixels[pixel];
                    require(color == colors[index], "wrong button color");
                }
            };
            checker.value("snapshot_length") = 527;
            ysfx_gfx_run(checker.effect.get());
            check_colors({0xcc6208, 0xcc6208, 0xcc6208, 0xcc6208});
            checker.value("grab_state") = 1;
            ysfx_gfx_run(checker.effect.get());
            check_colors({0x59402a, 0x533d2b, 0x4c3b2c, 0x4c3b2c});
            checker.value("grab_state") = 0;
            checker.value("snapshot_length") = 0;
            ysfx_gfx_update_mouse(checker.effect.get(), 0, 500, 145, ysfx_button_left, 0, 0);
            ysfx_gfx_run(checker.effect.get());
            require(checker.value("mode_requested") == 2, "mode menu did not queue Multi");
            checker.block();
            require(checker.output == std::vector<Bytes>({Bytes{0xb0, 0x63, 0x3e}, Bytes{0xb0, 0x62, 0}, Bytes{0xb0, 0x06, 1}}),
                "Multi selection must send verified global-channel NRPN");
            require(checker.value("grab_mode") == 1, "Multi selection did not update plugin mode");
            checker.output.clear();
            checker.value("snapshot_length") = 527;
            ysfx_gfx_run(checker.effect.get());
            check_colors({0x498dc7, 0x498dc7, 0x498dc7, 0x498dc7});
            checker.value("grab_state") = 1;
            ysfx_gfx_run(checker.effect.get());
            check_colors({0x3a4a58, 0x3a4853, 0x3a454e, 0x3a424a});
            checker.value("grab_state") = 0;
            checker.value("snapshot_length") = 0;
            ysfx_gfx_update_mouse(checker.effect.get(), 0, 500, 145, 0, 0, 0);
            ysfx_gfx_run(checker.effect.get());
            selection = 1;
            ysfx_gfx_update_mouse(checker.effect.get(), 0, 500, 145, ysfx_button_left, 0, 0);
            ysfx_gfx_run(checker.effect.get());
            require(checker.value("mode_requested") == 1, "mode menu did not queue Single");
            checker.block();
            require(checker.output == std::vector<Bytes>({Bytes{0xb1, 0x63, 0x3e}, Bytes{0xb1, 0x62, 0}, Bytes{0xb1, 0x06, 0}}),
                "Single selection must send verified Part A channel NRPN");
            require(checker.value("grab_mode") == 0, "Single selection did not update plugin mode");
            checker.output.clear();
            checker.block();
            require(checker.output.empty(), "mode switch repeated without selection");
            checker.value("grab_state") = 1;
            checker.value("mode_requested") = 2;
            checker.block();
            require(checker.output.empty() && checker.value("grab_mode") == 0,
                "mode switch overlapped Grab");
        }
        for (int mode = 0; mode < 4; ++mode) {
            Effect checker(argv[1]);
            checker.startup();
            checker.send({240, 126, 0, 6, 2, 0, 32, 41, 51, 1, 0, 0, 2, 2, 0, 0, 247});
            require(checker.value("connection_state") == 2, "identity rejected");
            checker.value("grab_mode") = mode != 0;
            double selected_grab_mode = checker.value("grab_mode");
            checker.value("grab_requested") = 1;
            checker.block();
            Bytes query{240, 0, 32, 41, 1, 17, 1, 51, static_cast<uint8_t>(mode ? 66 : 64), 0, 0, 0, 0, 0, 247};
            if (checker.output != std::vector<Bytes>({query})) {
                std::cerr << "mode=" << mode << " selected=" << selected_grab_mode
                    << " current=" << checker.value("grab_mode") << " requested=" << checker.value("requested_mode")
                    << " state=" << checker.value("grab_state") << " output:";
                for (const auto &event : checker.output)
                    for (auto byte : event) std::cerr << ' ' << std::hex << static_cast<int>(byte);
                std::cerr << std::dec << '\n';
            }
            require(checker.output == std::vector<Bytes>({query}), "wrong Grab query");
            checker.output.clear();
            Bytes dump = snapshot(mode != 0, mode - 1);
            Bytes other = snapshot(mode == 0, 0);
            checker.send(other);
            require(checker.value("snapshot_length") == 0, "Grab accepted wrong mode");
            for (size_t position = 0; position < dump.size(); position += 13) {
                size_t end = std::min(position + 13, dump.size());
                checker.send(Bytes(dump.begin() + position, dump.begin() + end));
                if (position == 0) checker.send({248});
            }
            auto saved_bytes = checker.bytes(71000, dump.size());
            require(std::equal(dump.begin(), dump.end(), saved_bytes.begin()), "Grab changed bytes");
            require(checker.value("grab_state") == 2, "Grab failed");
            require(checker.value("snapshot_mode") == (mode != 0), "wrong snapshot mode");
            auto name_bytes = checker.bytes(78000, static_cast<int>(checker.value("patch_name_length")));
            std::string decoded_name;
            for (double byte : name_bytes) decoded_name += static_cast<char>(byte);
            require(decoded_name == (mode ? "TEST MULTI" : "TEST SINGLE"), "wrong patch name");
            if (mode) require(checker.value("snapshot_subtype") == mode - 1, "wrong Multi subtype");
            require(checker.output == std::vector<Bytes>({Bytes{248}}), "SysEx echoed or realtime lost");
            checker.output.clear();
            ysfx_state_u saved{ysfx_save_state(checker.effect.get())};
            Effect restored(argv[1]);
            require(ysfx_load_state(restored.effect.get(), saved.get()), "project load failed");
            require(restored.bytes(71000, dump.size()) == saved_bytes, "project snapshot changed");
            restored.startup();
            require(restored.value("grab_state") == 5, "project status missing");
            bridge.state(0);
            for (int operation : {1, 2, 3}) {
                restored.value("action_requested") = operation;
                restored.block();
                bridge.block();
                require(bridge.value("observed_state") == 2 && bridge.value("observed_operation") == operation,
                    "wrong mailbox operation");
                if (operation != 3)
                    require(bridge.bytes(0, dump.size()) == saved_bytes, "mailbox changed bytes");
                restored.value("grab_requested") = 1;
                restored.block();
                require(restored.output.empty(), "Grab overlapped file/send operation");
                if (operation == 3) bridge.send(dump); else bridge.state(4);
                restored.block();
                require(restored.value("action_state") == 1, "operation acknowledgement failed");
                require(restored.bytes(71000, dump.size()) == saved_bytes, "operation changed snapshot");
                require(restored.output.empty(), "operation automatically sent MIDI");
            }
            for (int result : {5, 6}) {
                restored.value("action_requested") = 3;
                restored.block();
                bridge.state(result);
                restored.block();
                require(restored.bytes(71000, dump.size()) == saved_bytes, "failed/cancelled Load lost snapshot");
            }
            restored.value("action_requested") = 3;
            restored.block();
            Bytes corrupt = dump;
            corrupt[20] = 128;
            bridge.send(corrupt);
            restored.block();
            require(restored.value("action_state") == 2 && restored.bytes(71000, dump.size()) == saved_bytes,
                "invalid loaded snapshot accepted");
            if (mode == 3) {
                std::vector<uint8_t> pixels(619 * 396 * 4);
                ysfx_gfx_config_t graphics{};
                graphics.pixel_width = 619;
                graphics.pixel_height = 396;
                graphics.pixels = pixels.data();
                graphics.scale_factor = 1;
                ysfx_gfx_setup(restored.effect.get(), &graphics);
                ysfx_gfx_run(restored.effect.get());
                double header_width = 0, header_height = 0;
                header_width = restored.value("gfx_w");
                header_height = restored.value("gfx_h");
                require(header_width == 619 && header_height == 396, "graphics did not run");
                std::ofstream image("/tmp/summit-exchange.ppm", std::ios::binary);
                image << "P6\n619 396\n255\n";
                for (size_t index = 0; index < pixels.size(); index += 4) {
                    image.put(pixels[index + 2]); image.put(pixels[index + 1]); image.put(pixels[index]);
                }
            }
        }
        std::cout << "PASS: identity, Single/Layer/Split/Dual capture, fragmentation, mode/name, project state, Send/Load/Save mailbox, no auto-send, overlap, cancellation, corrupt data, graphics\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}