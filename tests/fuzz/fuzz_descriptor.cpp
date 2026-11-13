// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/*
 * One bounded libFuzzer process exercises the four pure paths: descriptor
 * construction, report serialization, raw-descriptor validation, and the parser
 * of the frames a board sends. It opens no serial port and asserts only memory
 * safety.
 */

#include "api/internal.hpp"
#include "classes/hid/hid_class.hpp"
#include "hid/descriptor_builder.hpp"
#include "hid/usages.hpp"
#include "hid/validator.hpp"
#include "mimusb.h"
#include "transport/link.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace {

void fuzz_builder(const std::uint8_t* data, const std::size_t size) {
    std::array<std::uint8_t, 512U> descriptor{};
    mimusb::hid::ReportLayout layout{};
    mimusb::hid::DescriptorBuilder builder(descriptor.data(), descriptor.size(), &layout);
    const std::uint8_t selector = size == 0U ? 0U : data[0];
    static_cast<void>(builder.begin_application(mimusb::hid::usage::page_generic_desktop,
                                                mimusb::hid::usage::mouse));
    static_cast<void>(builder.set_report_id((selector & 1U) != 0U,
                                            static_cast<std::uint8_t>(1U + selector % 254U)));
    static_cast<void>(builder.begin_physical(mimusb::hid::usage::page_generic_desktop,
                                             mimusb::hid::usage::pointer));
    static_cast<void>(builder.variable_range(mimusb::hid::usage::page_button, 1U,
                                             static_cast<std::uint16_t>(1U + selector % 32U), 1U,
                                             mimusb::hid::FieldSemantic::buttons));
    static_cast<void>(builder.constant_padding(static_cast<std::uint16_t>(selector % 8U)));
    static_cast<void>(builder.variable(
        mimusb::hid::usage::page_generic_desktop, mimusb::hid::usage::x, -32767, 32767, 16U,
        (selector & 2U) != 0U, false, mimusb::hid::FieldSemantic::x, 0U));
    static_cast<void>(builder.variable(
        mimusb::hid::usage::page_generic_desktop, mimusb::hid::usage::y, -32767, 32767, 16U,
        (selector & 4U) != 0U, false, mimusb::hid::FieldSemantic::y, 0U));
    static_cast<void>(builder.end_collection());
    static_cast<void>(builder.end_collection());
    builder.finish();
    if (builder.good()) {
        static_cast<void>(
            mimusb::hid::validate_generated(descriptor.data(), builder.descriptor_size(), layout));
    }
}

void fuzz_serializer(const std::uint8_t* data, const std::size_t size) {
    mimusb_mouse_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.function.bcd_hid = 0x0111;
    options.button_count = 8U;
    options.x = {-32767, 32767, 16U, {}};
    options.y = {-32767, 32767, 16U, {}};
    options.enable_wheel = 1U;
    options.wheel = {-127, 127, 8U, {}};
    options.enable_pan = 1U;
    options.pan = {-127, 127, 8U, {}};
    mimusb_spec* spec = nullptr;
    if (mimusb_spec_create_mouse(&options, &spec) != MIMUSB_OK || spec == nullptr) {
        return;
    }

    mimusb::detail::MouseState state{};
    state.buttons.resize(options.button_count);
    for (std::size_t index = 0U; index < state.buttons.size() && index < size; ++index) {
        state.buttons[index] = static_cast<std::uint8_t>(data[index] & 1U);
    }
    if (size > 8U)
        state.dx = static_cast<std::int8_t>(data[8U]);
    if (size > 9U)
        state.dy = static_cast<std::int8_t>(data[9U]);
    if (size > 10U)
        state.wheel = static_cast<std::int8_t>(data[10U]);
    if (size > 11U)
        state.pan = static_cast<std::int8_t>(data[11U]);
    mimusb_node node{};
    mimusb::detail::attach_hid(&node, spec);
    mimusb::detail::hid_node(&node)->state = std::move(state);
    std::array<std::uint8_t, 256U> report{};
    std::size_t report_length = 0U;
    if (mimusb::detail::serialize_node(mimusb::detail::hid_node(&node), report.data(),
                                       report.size(), &report_length) == MIMUSB_OK) {
        static_cast<void>(mimusb::detail::validate_serialized_report(mimusb::detail::hid_spec(spec),
                                                                     report.data(), report_length));
    }
    mimusb_spec_release(spec);
}

void fuzz_raw_validator(const std::uint8_t* data, const std::size_t size) {
    if (data == nullptr || size == 0U) {
        return;
    }

    constexpr std::size_t maximum_reports = 4U;
    std::array<mimusb_hid_report, maximum_reports> reports{};
    const std::size_t report_count = 1U + static_cast<std::size_t>(data[0] % maximum_reports);
    std::size_t cursor = 1U;
    for (std::size_t index = 0U; index < report_count; ++index) {
        const std::uint8_t report_id = cursor < size ? data[cursor++] : 0U;
        const std::uint8_t flags = cursor < size ? data[cursor++] : 0U;
        const std::uint8_t length = cursor < size ? data[cursor++] : 0U;
        reports[index].report_id.enabled = flags & 1U;
        reports[index].report_id.value =
            reports[index].report_id.enabled == 1U
                ? static_cast<std::uint8_t>(report_id == 0U ? 1U : report_id)
                : 0U;
        reports[index].wire_length = static_cast<std::uint32_t>(length) + 1U;
    }
    const std::uint8_t* descriptor = cursor < size ? data + cursor : data;
    const std::size_t descriptor_length = cursor < size ? size - cursor : size;
    static_cast<void>(
        mimusb::hid::validate_raw(descriptor, descriptor_length, reports.data(), report_count));
}

void fuzz_frame_parser(const std::uint8_t* data, const std::size_t size) {
    // On the heap: a parser holds a 16 KiB ring.
    const auto parser = std::make_unique<mimusb::transport::FrameParser>();
    std::size_t frames = 0U;
    const auto count = [&frames](const mimusb::transport::Frame&) { ++frames; };
    // The input in two pieces, so a frame split anywhere is parsed.
    const std::size_t split = size / 2U;
    parser->feed(data, split, count);
    parser->feed(data + split, size - split, count);
    // An idle line abandons an incomplete frame one byte at a time.
    for (std::size_t guard = 0U; parser->pending() && guard < 2U * size + 1U; ++guard) {
        parser->idle(count);
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) {
    if (data == nullptr || size == 0U) {
        return 0;
    }
    fuzz_builder(data, size);
    fuzz_serializer(data, size);
    fuzz_raw_validator(data, size);
    fuzz_frame_parser(data, size);
    return 0;
}
