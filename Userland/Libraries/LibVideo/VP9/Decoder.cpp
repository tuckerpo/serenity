/*
 * Copyright (c) 2021, Hunter Salyer <thefalsehonesty@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Decoder.h"

namespace Video::VP9 {

#define RESERVED_ZERO                  \
    if (m_bit_stream->read_bit() != 0) \
    return false

#define SAFE_CALL(call)       \
    if (!(call)) [[unlikely]] \
    return false

Decoder::Decoder()
    : m_probability_tables(make<ProbabilityTables>())
    , m_tree_parser(make<TreeParser>(*this))
{
}

bool Decoder::parse_frame(ByteBuffer const& frame_data)
{
    m_bit_stream = make<BitStream>(frame_data.data(), frame_data.size());
    m_syntax_element_counter = make<SyntaxElementCounter>();

    SAFE_CALL(uncompressed_header());
    dbgln("Finished reading uncompressed header");
    SAFE_CALL(trailing_bits());
    if (m_header_size_in_bytes == 0) {
        // FIXME: Do we really need to read all of these bits?
        // while (m_bit_stream->get_position() < m_start_bit_pos + (8 * frame_data.size()))
        //     RESERVED_ZERO;
        dbgln("No header");
        return true;
    }
    m_probability_tables->load_probs(m_frame_context_idx);
    m_probability_tables->load_probs2(m_frame_context_idx);
    m_syntax_element_counter->clear_counts();

    SAFE_CALL(m_bit_stream->init_bool(m_header_size_in_bytes));
    dbgln("Reading compressed header");
    SAFE_CALL(compressed_header());
    dbgln("Finished reading compressed header");
    SAFE_CALL(m_bit_stream->exit_bool());
    dbgln("Finished reading frame!");

    SAFE_CALL(decode_tiles());
    return true;
}

bool Decoder::uncompressed_header()
{
    auto frame_marker = m_bit_stream->read_f(2);
    if (frame_marker != 2)
        return false;
    auto profile_low_bit = m_bit_stream->read_bit();
    auto profile_high_bit = m_bit_stream->read_bit();
    m_profile = (profile_high_bit << 1u) + profile_low_bit;
    if (m_profile == 3)
        RESERVED_ZERO;
    auto show_existing_frame = m_bit_stream->read_bit();
    if (show_existing_frame) {
        m_frame_to_show_map_index = m_bit_stream->read_f(3);
        m_header_size_in_bytes = 0;
        m_refresh_frame_flags = 0;
        m_loop_filter_level = 0;
        return true;
    }

    m_last_frame_type = m_frame_type;
    m_frame_type = read_frame_type();
    m_show_frame = m_bit_stream->read_bit();
    m_error_resilient_mode = m_bit_stream->read_bit();

    if (m_frame_type == KeyFrame) {
        SAFE_CALL(frame_sync_code());
        SAFE_CALL(color_config());
        SAFE_CALL(frame_size());
        SAFE_CALL(render_size());
        m_refresh_frame_flags = 0xFF;
        m_frame_is_intra = true;
    } else {
        m_frame_is_intra = !m_show_frame && m_bit_stream->read_bit();

        if (!m_error_resilient_mode) {
            m_reset_frame_context = m_bit_stream->read_f(2);
        } else {
            m_reset_frame_context = 0;
        }

        if (m_frame_is_intra) {
            SAFE_CALL(frame_sync_code());
            if (m_profile > 0) {
                SAFE_CALL(color_config());
            } else {
                m_color_space = Bt601;
                m_subsampling_x = true;
                m_subsampling_y = true;
                m_bit_depth = 8;
            }

            m_refresh_frame_flags = m_bit_stream->read_f8();
            SAFE_CALL(frame_size());
            SAFE_CALL(render_size());
        } else {
            m_refresh_frame_flags = m_bit_stream->read_f8();
            for (auto i = 0; i < 3; i++) {
                m_ref_frame_idx[i] = m_bit_stream->read_f(3);
                m_ref_frame_sign_bias[LastFrame + i] = m_bit_stream->read_bit();
            }
            SAFE_CALL(frame_size_with_refs());
            m_allow_high_precision_mv = m_bit_stream->read_bit();
            SAFE_CALL(read_interpolation_filter());
        }
    }

    if (!m_error_resilient_mode) {
        m_refresh_frame_context = m_bit_stream->read_bit();
        m_frame_parallel_decoding_mode = m_bit_stream->read_bit();
    } else {
        m_refresh_frame_context = false;
        m_frame_parallel_decoding_mode = true;
    }

    m_frame_context_idx = m_bit_stream->read_f(2);
    if (m_frame_is_intra || m_error_resilient_mode) {
        SAFE_CALL(setup_past_independence());
        if (m_frame_type == KeyFrame || m_error_resilient_mode || m_reset_frame_context == 3) {
            for (auto i = 0; i < 4; i++) {
                m_probability_tables->save_probs(i);
            }
        } else if (m_reset_frame_context == 2) {
            m_probability_tables->save_probs(m_frame_context_idx);
        }
        m_frame_context_idx = 0;
    }

    SAFE_CALL(loop_filter_params());
    SAFE_CALL(quantization_params());
    SAFE_CALL(segmentation_params());
    SAFE_CALL(tile_info());

    m_header_size_in_bytes = m_bit_stream->read_f16();

    return true;
}

bool Decoder::frame_sync_code()
{
    if (m_bit_stream->read_byte() != 0x49)
        return false;
    if (m_bit_stream->read_byte() != 0x83)
        return false;
    return m_bit_stream->read_byte() == 0x42;
}

bool Decoder::color_config()
{
    if (m_profile >= 2) {
        m_bit_depth = m_bit_stream->read_bit() ? 12 : 10;
    } else {
        m_bit_depth = 8;
    }

    auto color_space = m_bit_stream->read_f(3);
    if (color_space > RGB)
        return false;
    m_color_space = static_cast<ColorSpace>(color_space);

    if (color_space != RGB) {
        m_color_range = read_color_range();
        if (m_profile == 1 || m_profile == 3) {
            m_subsampling_x = m_bit_stream->read_bit();
            m_subsampling_y = m_bit_stream->read_bit();
            RESERVED_ZERO;
        } else {
            m_subsampling_x = true;
            m_subsampling_y = true;
        }
    } else {
        m_color_range = FullSwing;
        if (m_profile == 1 || m_profile == 3) {
            m_subsampling_x = false;
            m_subsampling_y = false;
            RESERVED_ZERO;
        }
    }
    return true;
}

bool Decoder::frame_size()
{
    m_frame_width = m_bit_stream->read_f16() + 1;
    m_frame_height = m_bit_stream->read_f16() + 1;
    SAFE_CALL(compute_image_size());
    return true;
}

bool Decoder::render_size()
{
    if (m_bit_stream->read_bit()) {
        m_render_width = m_bit_stream->read_f16() + 1;
        m_render_height = m_bit_stream->read_f16() + 1;
    } else {
        m_render_width = m_frame_width;
        m_render_height = m_frame_height;
    }
    return true;
}

bool Decoder::frame_size_with_refs()
{
    bool found_ref;
    for (auto i = 0; i < 3; i++) {
        found_ref = m_bit_stream->read_bit();
        if (found_ref) {
            // TODO:
            //  - FrameWidth = RefFrameWidth[ref_frame_idx[ i] ];
            //  - FrameHeight = RefFrameHeight[ref_frame_idx[ i] ];
            break;
        }
    }

    if (!found_ref) {
        SAFE_CALL(frame_size());
    } else {
        SAFE_CALL(compute_image_size());
    }

    SAFE_CALL(render_size());
    return true;
}

bool Decoder::compute_image_size()
{
    m_mi_cols = (m_frame_width + 7u) >> 3u;
    m_mi_rows = (m_frame_height + 7u) >> 3u;
    m_sb64_cols = (m_mi_cols + 7u) >> 3u;
    m_sb64_rows = (m_mi_rows + 7u) >> 3u;
    return true;
}

bool Decoder::read_interpolation_filter()
{
    if (m_bit_stream->read_bit()) {
        m_interpolation_filter = Switchable;
    } else {
        m_interpolation_filter = literal_to_type[m_bit_stream->read_f(2)];
    }
    return true;
}

bool Decoder::loop_filter_params()
{
    m_loop_filter_level = m_bit_stream->read_f(6);
    m_loop_filter_sharpness = m_bit_stream->read_f(3);
    m_loop_filter_delta_enabled = m_bit_stream->read_bit();
    if (m_loop_filter_delta_enabled) {
        if (m_bit_stream->read_bit()) {
            for (auto i = 0; i < 4; i++) {
                if (m_bit_stream->read_bit()) {
                    // TODO: loop_filter_ref_deltas[i] = s(6);
                }
            }
            for (auto i = 0; i < 2; i++) {
                if (m_bit_stream->read_bit()) {
                    // TODO: loop_filter_mode_deltas[i] = s(6);
                }
            }
        }
    }
    return true;
}

bool Decoder::quantization_params()
{
    auto base_q_idx = m_bit_stream->read_byte();
    auto delta_q_y_dc = read_delta_q();
    auto delta_q_uv_dc = read_delta_q();
    auto delta_q_uv_ac = read_delta_q();
    m_lossless = base_q_idx == 0 && delta_q_y_dc == 0 && delta_q_uv_dc == 0 && delta_q_uv_ac == 0;
    return true;
}

i8 Decoder::read_delta_q()
{
    if (m_bit_stream->read_bit())
        return m_bit_stream->read_s(4);
    return 0;
}

bool Decoder::segmentation_params()
{
    m_segmentation_enabled = m_bit_stream->read_bit();
    if (!m_segmentation_enabled)
        return true;

    m_segmentation_update_map = m_bit_stream->read_bit();
    if (m_segmentation_update_map) {
        for (auto i = 0; i < 7; i++)
            m_segmentation_tree_probs[i] = read_prob();
        m_segmentation_temporal_update = m_bit_stream->read_bit();
        for (auto i = 0; i < 3; i++)
            m_segmentation_pred_prob[i] = m_segmentation_temporal_update ? read_prob() : 255;
    }

    SAFE_CALL(m_bit_stream->read_bit());

    m_segmentation_abs_or_delta_update = m_bit_stream->read_bit();
    for (auto i = 0; i < MAX_SEGMENTS; i++) {
        for (auto j = 0; j < SEG_LVL_MAX; j++) {
            auto feature_value = 0;
            auto feature_enabled = m_bit_stream->read_bit();
            m_feature_enabled[i][j] = feature_enabled;
            if (feature_enabled) {
                auto bits_to_read = segmentation_feature_bits[j];
                feature_value = m_bit_stream->read_f(bits_to_read);
                if (segmentation_feature_signed[j]) {
                    if (m_bit_stream->read_bit())
                        feature_value = -feature_value;
                }
            }
            m_feature_data[i][j] = feature_value;
        }
    }
    return true;
}

u8 Decoder::read_prob()
{
    if (m_bit_stream->read_bit())
        return m_bit_stream->read_byte();
    return 255;
}

bool Decoder::tile_info()
{
    auto min_log2_tile_cols = calc_min_log2_tile_cols();
    auto max_log2_tile_cols = calc_max_log2_tile_cols();
    m_tile_cols_log2 = min_log2_tile_cols;
    while (m_tile_cols_log2 < max_log2_tile_cols) {
        if (m_bit_stream->read_bit())
            m_tile_cols_log2++;
        else
            break;
    }
    m_tile_rows_log2 = m_bit_stream->read_bit();
    if (m_tile_rows_log2) {
        m_tile_rows_log2 += m_bit_stream->read_bit();
    }
    return true;
}

u16 Decoder::calc_min_log2_tile_cols()
{
    auto min_log_2 = 0u;
    while ((u8)(MAX_TILE_WIDTH_B64 << min_log_2) < m_sb64_cols)
        min_log_2++;
    return min_log_2;
}

u16 Decoder::calc_max_log2_tile_cols()
{
    u16 max_log_2 = 1;
    while ((m_sb64_cols >> max_log_2) >= MIN_TILE_WIDTH_B64)
        max_log_2++;
    return max_log_2 - 1;
}

bool Decoder::setup_past_independence()
{
    for (auto i = 0; i < 8; i++) {
        for (auto j = 0; j < 4; j++) {
            m_feature_data[i][j] = 0;
            m_feature_enabled[i][j] = false;
        }
    }
    m_segmentation_abs_or_delta_update = false;
    for (auto row = 0u; row < m_mi_rows; row++) {
        for (auto col = 0u; col < m_mi_cols; col++) {
            // TODO: m_prev_segment_ids[row][col] = 0;
        }
    }
    m_loop_filter_delta_enabled = true;
    m_loop_filter_ref_deltas[IntraFrame] = 1;
    m_loop_filter_ref_deltas[LastFrame] = 0;
    m_loop_filter_ref_deltas[GoldenFrame] = -1;
    m_loop_filter_ref_deltas[AltRefFrame] = -1;
    for (auto i = 0; i < 2; i++) {
        m_loop_filter_mode_deltas[i] = 0;
    }
    m_probability_tables->reset_probs();
    return true;
}

bool Decoder::trailing_bits()
{
    while (m_bit_stream->get_position() & 7u)
        RESERVED_ZERO;
    return true;
}

bool Decoder::compressed_header()
{
    SAFE_CALL(read_tx_mode());
    if (m_tx_mode == TXModeSelect) {
        SAFE_CALL(tx_mode_probs());
    }
    SAFE_CALL(read_coef_probs());
    SAFE_CALL(read_skip_prob());
    if (!m_frame_is_intra) {
        SAFE_CALL(read_inter_mode_probs());
        if (m_interpolation_filter == Switchable) {
            SAFE_CALL(read_interp_filter_probs());
        }
        SAFE_CALL(read_is_inter_probs());
        SAFE_CALL(frame_reference_mode());
        SAFE_CALL(frame_reference_mode_probs());
        SAFE_CALL(read_y_mode_probs());
        SAFE_CALL(read_partition_probs());
        SAFE_CALL(mv_probs());
    }
    return true;
}

bool Decoder::read_tx_mode()
{
    if (m_lossless) {
        m_tx_mode = Only_4x4;
    } else {
        auto tx_mode = m_bit_stream->read_literal(2);
        if (tx_mode == Allow_32x32) {
            tx_mode += m_bit_stream->read_literal(1);
        }
        m_tx_mode = static_cast<TXMode>(tx_mode);
    }
    return true;
}

bool Decoder::tx_mode_probs()
{
    auto& tx_probs = m_probability_tables->tx_probs();
    for (auto i = 0; i < TX_SIZE_CONTEXTS; i++) {
        for (auto j = 0; j < TX_SIZES - 3; j++) {
            tx_probs[TX_8x8][i][j] = diff_update_prob(tx_probs[TX_8x8][i][j]);
        }
    }
    for (auto i = 0; i < TX_SIZE_CONTEXTS; i++) {
        for (auto j = 0; j < TX_SIZES - 2; j++) {
            tx_probs[TX_16x16][i][j] = diff_update_prob(tx_probs[TX_16x16][i][j]);
        }
    }
    for (auto i = 0; i < TX_SIZE_CONTEXTS; i++) {
        for (auto j = 0; j < TX_SIZES - 1; j++) {
            tx_probs[TX_32x32][i][j] = diff_update_prob(tx_probs[TX_32x32][i][j]);
        }
    }
    return true;
}

u8 Decoder::diff_update_prob(u8 prob)
{
    if (m_bit_stream->read_bool(252)) {
        auto delta_prob = decode_term_subexp();
        prob = inv_remap_prob(delta_prob, prob);
    }
    return prob;
}

u8 Decoder::decode_term_subexp()
{
    if (m_bit_stream->read_literal(1) == 0)
        return m_bit_stream->read_literal(4);
    if (m_bit_stream->read_literal(1) == 0)
        return m_bit_stream->read_literal(4) + 16;
    if (m_bit_stream->read_literal(1) == 0)
        return m_bit_stream->read_literal(4) + 32;

    auto v = m_bit_stream->read_literal(7);
    if (v < 65)
        return v + 64;
    return (v << 1u) - 1 + m_bit_stream->read_literal(1);
}

u8 Decoder::inv_remap_prob(u8 delta_prob, u8 prob)
{
    u8 m = prob - 1;
    auto v = inv_map_table[delta_prob];
    if ((m << 1u) <= 255)
        return 1 + inv_recenter_nonneg(v, m);
    return 255 - inv_recenter_nonneg(v, 254 - m);
}

u8 Decoder::inv_recenter_nonneg(u8 v, u8 m)
{
    if (v > 2 * m)
        return v;
    if (v & 1u)
        return m - ((v + 1u) >> 1u);
    return m + (v >> 1u);
}

bool Decoder::read_coef_probs()
{
    m_max_tx_size = tx_mode_to_biggest_tx_size[m_tx_mode];
    for (auto tx_size = TX_4x4; tx_size <= m_max_tx_size; tx_size = static_cast<TXSize>(static_cast<int>(tx_size) + 1)) {
        auto update_probs = m_bit_stream->read_literal(1);
        if (update_probs == 1) {
            for (auto i = 0; i < 2; i++) {
                for (auto j = 0; j < 2; j++) {
                    for (auto k = 0; k < 6; k++) {
                        auto max_l = (k == 0) ? 3 : 6;
                        for (auto l = 0; l < max_l; l++) {
                            for (auto m = 0; m < 3; m++) {
                                auto& coef_probs = m_probability_tables->coef_probs()[tx_size];
                                coef_probs[i][j][k][l][m] = diff_update_prob(coef_probs[i][j][k][l][m]);
                            }
                        }
                    }
                }
            }
        }
    }
    return true;
}

bool Decoder::read_skip_prob()
{
    for (auto i = 0; i < SKIP_CONTEXTS; i++)
        m_probability_tables->skip_prob()[i] = diff_update_prob(m_probability_tables->skip_prob()[i]);
    return true;
}

bool Decoder::read_inter_mode_probs()
{
    for (auto i = 0; i < INTER_MODE_CONTEXTS; i++) {
        for (auto j = 0; j < INTER_MODES - 1; j++)
            m_probability_tables->inter_mode_probs()[i][j] = diff_update_prob(m_probability_tables->inter_mode_probs()[i][j]);
    }
    return true;
}

bool Decoder::read_interp_filter_probs()
{
    for (auto i = 0; i < INTERP_FILTER_CONTEXTS; i++) {
        for (auto j = 0; j < SWITCHABLE_FILTERS - 1; j++)
            m_probability_tables->interp_filter_probs()[i][j] = diff_update_prob(m_probability_tables->interp_filter_probs()[i][j]);
    }
    return true;
}

bool Decoder::read_is_inter_probs()
{
    for (auto i = 0; i < IS_INTER_CONTEXTS; i++)
        m_probability_tables->is_inter_prob()[i] = diff_update_prob(m_probability_tables->is_inter_prob()[i]);
    return true;
}

bool Decoder::frame_reference_mode()
{
    auto compound_reference_allowed = false;
    for (size_t i = 2; i <= REFS_PER_FRAME; i++) {
        if (m_ref_frame_sign_bias[i] != m_ref_frame_sign_bias[1])
            compound_reference_allowed = true;
    }
    if (compound_reference_allowed) {
        auto non_single_reference = m_bit_stream->read_literal(1);
        if (non_single_reference == 0) {
            m_reference_mode = SingleReference;
        } else {
            auto reference_select = m_bit_stream->read_literal(1);
            if (reference_select == 0)
                m_reference_mode = CompoundReference;
            else
                m_reference_mode = ReferenceModeSelect;
            SAFE_CALL(setup_compound_reference_mode());
        }
    } else {
        m_reference_mode = SingleReference;
    }
    return true;
}

bool Decoder::frame_reference_mode_probs()
{
    if (m_reference_mode == ReferenceModeSelect) {
        for (auto i = 0; i < COMP_MODE_CONTEXTS; i++) {
            auto& comp_mode_prob = m_probability_tables->comp_mode_prob();
            comp_mode_prob[i] = diff_update_prob(comp_mode_prob[i]);
        }
    }
    if (m_reference_mode != CompoundReference) {
        for (auto i = 0; i < REF_CONTEXTS; i++) {
            auto& single_ref_prob = m_probability_tables->single_ref_prob();
            single_ref_prob[i][0] = diff_update_prob(single_ref_prob[i][0]);
            single_ref_prob[i][1] = diff_update_prob(single_ref_prob[i][1]);
        }
    }
    if (m_reference_mode != SingleReference) {
        for (auto i = 0; i < REF_CONTEXTS; i++) {
            auto& comp_ref_prob = m_probability_tables->comp_ref_prob();
            comp_ref_prob[i] = diff_update_prob(comp_ref_prob[i]);
        }
    }
    return true;
}

bool Decoder::read_y_mode_probs()
{
    for (auto i = 0; i < BLOCK_SIZE_GROUPS; i++) {
        for (auto j = 0; j < INTRA_MODES - 1; j++) {
            auto& y_mode_probs = m_probability_tables->y_mode_probs();
            y_mode_probs[i][j] = diff_update_prob(y_mode_probs[i][j]);
        }
    }
    return true;
}

bool Decoder::read_partition_probs()
{
    for (auto i = 0; i < PARTITION_CONTEXTS; i++) {
        for (auto j = 0; j < PARTITION_TYPES - 1; j++) {
            auto& partition_probs = m_probability_tables->partition_probs();
            partition_probs[i][j] = diff_update_prob(partition_probs[i][j]);
        }
    }
    return true;
}

bool Decoder::mv_probs()
{
    for (auto j = 0; j < MV_JOINTS - 1; j++) {
        auto& mv_joint_probs = m_probability_tables->mv_joint_probs();
        mv_joint_probs[j] = update_mv_prob(mv_joint_probs[j]);
    }

    for (auto i = 0; i < 2; i++) {
        auto& mv_sign_prob = m_probability_tables->mv_sign_prob();
        mv_sign_prob[i] = update_mv_prob(mv_sign_prob[i]);
        for (auto j = 0; j < MV_CLASSES - 1; j++) {
            auto& mv_class_probs = m_probability_tables->mv_class_probs();
            mv_class_probs[i][j] = update_mv_prob(mv_class_probs[i][j]);
        }
        auto& mv_class0_bit_prob = m_probability_tables->mv_class0_bit_prob();
        mv_class0_bit_prob[i] = update_mv_prob(mv_class0_bit_prob[i]);
        for (auto j = 0; j < MV_OFFSET_BITS; j++) {
            auto& mv_bits_prob = m_probability_tables->mv_bits_prob();
            mv_bits_prob[i][j] = update_mv_prob(mv_bits_prob[i][j]);
        }
    }

    for (auto i = 0; i < 2; i++) {
        for (auto j = 0; j < CLASS0_SIZE; j++) {
            for (auto k = 0; k < MV_FR_SIZE - 1; k++) {
                auto& mv_class0_fr_probs = m_probability_tables->mv_class0_fr_probs();
                mv_class0_fr_probs[i][j][k] = update_mv_prob(mv_class0_fr_probs[i][j][k]);
            }
        }
        for (auto k = 0; k < MV_FR_SIZE - 1; k++) {
            auto& mv_fr_probs = m_probability_tables->mv_fr_probs();
            mv_fr_probs[i][k] = update_mv_prob(mv_fr_probs[i][k]);
        }
    }

    if (m_allow_high_precision_mv) {
        for (auto i = 0; i < 2; i++) {
            auto& mv_class0_hp_prob = m_probability_tables->mv_class0_hp_prob();
            auto& mv_hp_prob = m_probability_tables->mv_hp_prob();
            mv_class0_hp_prob[i] = update_mv_prob(mv_class0_hp_prob[i]);
            mv_hp_prob[i] = update_mv_prob(mv_hp_prob[i]);
        }
    }

    return true;
}

u8 Decoder::update_mv_prob(u8 prob)
{
    if (m_bit_stream->read_bool(252)) {
        return (m_bit_stream->read_literal(7) << 1u) | 1u;
    }
    return prob;
}

bool Decoder::setup_compound_reference_mode()
{
    if (m_ref_frame_sign_bias[LastFrame] == m_ref_frame_sign_bias[GoldenFrame]) {
        m_comp_fixed_ref = AltRefFrame;
        m_comp_var_ref[0] = LastFrame;
        m_comp_var_ref[1] = GoldenFrame;
    } else if (m_ref_frame_sign_bias[LastFrame] == m_ref_frame_sign_bias[AltRefFrame]) {
        m_comp_fixed_ref = GoldenFrame;
        m_comp_var_ref[0] = LastFrame;
        m_comp_var_ref[1] = AltRefFrame;
    } else {
        m_comp_fixed_ref = LastFrame;
        m_comp_var_ref[0] = GoldenFrame;
        m_comp_var_ref[1] = AltRefFrame;
    }
    return true;
}

bool Decoder::decode_tiles()
{
    auto tile_cols = 1 << m_tile_cols_log2;
    auto tile_rows = 1 << m_tile_rows_log2;
    SAFE_CALL(clear_above_context());
    for (auto tile_row = 0; tile_row < tile_rows; tile_row++) {
        for (auto tile_col = 0; tile_col < tile_cols; tile_col++) {
            auto last_tile = (tile_row == tile_rows - 1) && (tile_col == tile_cols - 1);
            // FIXME: Spec has `sz -= tile_size + 4`, but I think we don't need this because our bit stream manages how much data we have left?
            auto tile_size = last_tile ? m_bit_stream->bytes_remaining() : m_bit_stream->read_f(32);
            m_mi_row_start = get_tile_offset(tile_row, m_mi_rows, m_tile_rows_log2);
            m_mi_row_end = get_tile_offset(tile_row + 1, m_mi_rows, m_tile_rows_log2);
            m_mi_col_start = get_tile_offset(tile_col, m_mi_cols, m_tile_cols_log2);
            m_mi_col_end = get_tile_offset(tile_col + 1, m_mi_cols, m_tile_cols_log2);
            SAFE_CALL(m_bit_stream->init_bool(tile_size));
            SAFE_CALL(decode_tile());
            SAFE_CALL(m_bit_stream->exit_bool());
        }
    }

    return true;
}

template<typename T>
void clear_context(T* context, size_t size)
{
    if (!(*context))
        *context = static_cast<T>(malloc(size));
    else
        __builtin_memset(*context, 0, size);
}

bool Decoder::clear_above_context()
{
    clear_context(&m_above_nonzero_context, sizeof(u8) * 3 * m_mi_cols * 2);
    clear_context(&m_above_seg_pred_context, sizeof(u8) * m_mi_cols);
    clear_context(&m_above_partition_context, sizeof(u8) * m_sb64_cols * 8);
    return true;
}

u32 Decoder::get_tile_offset(u32 tile_num, u32 mis, u32 tile_size_log2)
{
    u32 super_blocks = (mis + 7) >> 3u;
    u32 offset = ((tile_num * super_blocks) >> tile_size_log2) << 3u;
    return min(offset, mis);
}

bool Decoder::decode_tile()
{
    for (auto row = m_mi_row_start; row < m_mi_row_end; row += 8) {
        SAFE_CALL(clear_left_context());
        m_row = row;
        for (auto col = m_mi_col_start; col < m_mi_col_end; col += 8) {
            m_col = col;
            SAFE_CALL(decode_partition(row, col, Block_64x64));
        }
    }
    return true;
}

bool Decoder::clear_left_context()
{
    clear_context(&m_left_nonzero_context, sizeof(u8) * 3 * m_mi_rows * 2);
    clear_context(&m_left_seg_pred_context, sizeof(u8) * m_mi_rows);
    clear_context(&m_left_partition_context, sizeof(u8) * m_sb64_rows * 8);
    return true;
}

bool Decoder::decode_partition(u32 row, u32 col, u8 block_subsize)
{
    if (row >= m_mi_rows || col >= m_mi_cols)
        return false;
    m_block_subsize = block_subsize;
    m_num_8x8 = num_8x8_blocks_wide_lookup[block_subsize];
    auto half_block_8x8 = m_num_8x8 >> 1;
    m_has_rows = (row + half_block_8x8) < m_mi_rows;
    m_has_cols = (col + half_block_8x8) < m_mi_cols;

    auto partition = m_tree_parser->parse_tree(SyntaxElementType::Partition);
    dbgln("Parsed partition value {}", partition);

    auto subsize = subsize_lookup[partition][block_subsize];
    if (subsize < Block_8x8 || partition == PartitionNone) {
        SAFE_CALL(decode_block(row, col, subsize));
    } else if (partition == PartitionHorizontal) {
        SAFE_CALL(decode_block(row, col, subsize));
        // FIXME: if (hasRows)
        //            decode_block(r + halfBlock8x8, c, subsize)
    }
    // FIXME: Finish implementing partition decoding

    return true;
}

bool Decoder::decode_block(u32 row, u32 col, u8 subsize)
{
    m_mi_row = row;
    m_mi_col = col;
    m_mi_size = subsize;
    m_available_u = row > 0;
    m_available_l = col > m_mi_col_start;
    SAFE_CALL(mode_info());
    // FIXME: Finish implementing
    return true;
}

bool Decoder::mode_info()
{
    if (m_frame_is_intra)
        return intra_frame_mode_info();
    return inter_frame_mode_info();
}

bool Decoder::intra_frame_mode_info()
{
    SAFE_CALL(intra_segment_id());
    SAFE_CALL(read_skip());
    SAFE_CALL(read_tx_size(true));
    m_ref_frame[0] = IntraFrame;
    m_ref_frame[1] = None;
    m_is_inter = false;
    if (m_mi_size >= Block_8x8) {
        m_default_intra_mode = m_tree_parser->parse_tree<IntraMode>(SyntaxElementType::DefaultIntraMode);
        m_y_mode = m_default_intra_mode;
        for (auto b = 0; b < 4; b++)
            m_sub_modes[b] = m_y_mode;
    } else {
        m_num_4x4_w = num_4x4_blocks_wide_lookup[m_mi_size];
        m_num_4x4_h = num_4x4_blocks_high_lookup[m_mi_size];
        for (auto idy = 0; idy < 2; idy += m_num_4x4_h) {
            for (auto idx = 0; idx < 2; idx += m_num_4x4_w) {
                m_default_intra_mode = m_tree_parser->parse_tree<IntraMode>(SyntaxElementType::DefaultIntraMode);
                for (auto y = 0; y < m_num_4x4_h; y++) {
                    for (auto x = 0; x < m_num_4x4_w; x++) {
                        auto index = (idy + y) * 2 + idx + x;
                        if (index > 3)
                            dbgln("Trying to access index {} on m_sub_modes", index);
                        m_sub_modes[index] = m_default_intra_mode;
                    }
                }
            }
        }
        m_y_mode = m_default_intra_mode;
    }
    m_uv_mode = m_tree_parser->parse_tree<u8>(SyntaxElementType::DefaultUVMode);
    return true;
}

bool Decoder::intra_segment_id()
{
    if (m_segmentation_enabled && m_segmentation_update_map) {
        m_segment_id = m_tree_parser->parse_tree<u8>(SyntaxElementType::SegmentID);
    } else {
        m_segment_id = 0;
    }
    return true;
}

bool Decoder::read_skip()
{
    if (seg_feature_active(SEG_LVL_SKIP)) {
        m_skip = true;
    } else {
        m_skip = m_tree_parser->parse_tree<bool>(SyntaxElementType::Skip);
    }
    return true;
}

bool Decoder::seg_feature_active(u8 feature)
{
    return m_segmentation_enabled && m_feature_enabled[m_segment_id][feature];
}

bool Decoder::read_tx_size(bool allow_select)
{
    m_max_tx_size = max_txsize_lookup[m_mi_size];
    if (allow_select && m_tx_mode == TXModeSelect && m_mi_size >= Block_8x8) {
        m_tx_size = m_tree_parser->parse_tree<TXSize>(SyntaxElementType::TXSize);
    } else {
        m_tx_size = min(m_max_tx_size, tx_mode_to_biggest_tx_size[m_tx_mode]);
    }
    return true;
}

bool Decoder::inter_frame_mode_info()
{
    m_left_ref_frame[0] = m_available_l ? m_ref_frames[m_mi_row][m_mi_col - 1][0] : IntraFrame;
    m_above_ref_frame[0] = m_available_u ? m_ref_frames[m_mi_row - 1][m_mi_col][0] : IntraFrame;
    m_left_ref_frame[1] = m_available_l ? m_ref_frames[m_mi_row][m_mi_col - 1][1] : None;
    m_above_ref_frame[1] = m_available_u ? m_ref_frames[m_mi_row - 1][m_mi_col][1] : None;
    m_left_intra = m_left_ref_frame[0] <= IntraFrame;
    m_above_intra = m_above_ref_frame[0] <= IntraFrame;
    m_left_single = m_left_ref_frame[1] <= None;
    m_above_single = m_above_ref_frame[1] <= None;
    SAFE_CALL(inter_segment_id());
    SAFE_CALL(read_skip());
    SAFE_CALL(read_is_inter());
    SAFE_CALL(read_tx_size(!m_skip || !m_is_inter));
    if (m_is_inter) {
        SAFE_CALL(inter_block_mode_info());
    } else {
        SAFE_CALL(intra_block_mode_info());
    }
    return true;
}

bool Decoder::inter_segment_id()
{
    if (!m_segmentation_enabled) {
        m_segment_id = 0;
        return true;
    }
    auto predicted_segment_id = get_segment_id();
    if (!m_segmentation_update_map) {
        m_segment_id = predicted_segment_id;
        return true;
    }
    if (!m_segmentation_temporal_update) {
        m_segment_id = m_tree_parser->parse_tree<u8>(SyntaxElementType::SegmentID);
        return true;
    }

    auto seg_id_predicted = m_tree_parser->parse_tree<bool>(SyntaxElementType::SegIDPredicted);
    if (seg_id_predicted)
        m_segment_id = predicted_segment_id;
    else
        m_segment_id = m_tree_parser->parse_tree<u8>(SyntaxElementType::SegmentID);
    for (auto i = 0u; i < num_8x8_blocks_wide_lookup[m_mi_size]; i++)
        m_above_seg_pred_context[m_mi_col + i] = seg_id_predicted;
    for (auto i = 0u; i < num_8x8_blocks_high_lookup[m_mi_size]; i++)
        m_left_seg_pred_context[m_mi_row + i] = seg_id_predicted;
    return true;
}

u8 Decoder::get_segment_id()
{
    auto bw = num_8x8_blocks_wide_lookup[m_mi_size];
    auto bh = num_8x8_blocks_high_lookup[m_mi_size];
    auto xmis = min(m_mi_cols - m_mi_col, (u32)bw);
    auto ymis = min(m_mi_rows - m_mi_row, (u32)bh);
    u8 segment = 7;
    for (auto y = 0u; y < ymis; y++) {
        for (auto x = 0u; x < xmis; x++) {
            segment = min(segment, m_prev_segment_ids[m_mi_row + y][m_mi_col + x]);
        }
    }
    return segment;
}

bool Decoder::read_is_inter()
{
    if (seg_feature_active(SEG_LVL_REF_FRAME))
        m_is_inter = m_feature_data[m_segment_id][SEG_LVL_REF_FRAME] != IntraFrame;
    else
        m_is_inter = m_tree_parser->parse_tree<bool>(SyntaxElementType::IsInter);
    return true;
}

bool Decoder::intra_block_mode_info()
{
    m_ref_frame[0] = IntraFrame;
    m_ref_frame[1] = None;
    if (m_mi_size >= Block_8x8) {
        m_y_mode = m_tree_parser->parse_tree<u8>(SyntaxElementType::IntraMode);
        for (auto b = 0; b < 4; b++)
            m_sub_modes[b] = m_y_mode;
    } else {
        m_num_4x4_w = num_4x4_blocks_wide_lookup[m_mi_size];
        m_num_4x4_h = num_4x4_blocks_high_lookup[m_mi_size];
        u8 sub_intra_mode;
        for (auto idy = 0; idy < 2; idy += m_num_4x4_h) {
            for (auto idx = 0; idx < 2; idx += m_num_4x4_w) {
                sub_intra_mode = m_tree_parser->parse_tree<u8>(SyntaxElementType::SubIntraMode);
                for (auto y = 0; y < m_num_4x4_h; y++) {
                    for (auto x = 0; x < m_num_4x4_w; x++) {
                        m_sub_modes[(idy + y) * 2 + idx + x] = sub_intra_mode;
                    }
                }
            }
        }
        m_y_mode = sub_intra_mode;
    }
    m_uv_mode = m_tree_parser->parse_tree<u8>(SyntaxElementType::UVMode);
    return true;
}

bool Decoder::inter_block_mode_info()
{
    SAFE_CALL(read_ref_frames());
    for (auto j = 0; j < 2; j++) {
        if (m_ref_frame[j] > IntraFrame) {
            SAFE_CALL(find_mv_refs(m_ref_frame[j], -1));
            SAFE_CALL(find_best_ref_mvs(j));
        }
    }
    auto is_compound = m_ref_frame[1] > IntraFrame;
    if (seg_feature_active(SEG_LVL_SKIP)) {
        m_y_mode = ZeroMv;
    } else if (m_mi_size >= Block_8x8) {
        auto inter_mode = m_tree_parser->parse_tree(SyntaxElementType::InterMode);
        m_y_mode = NearestMv + inter_mode;
    }
    if (m_interpolation_filter == Switchable)
        m_interp_filter = m_tree_parser->parse_tree<InterpolationFilter>(SyntaxElementType::InterpFilter);
    else
        m_interp_filter = m_interpolation_filter;
    if (m_mi_size < Block_8x8) {
        m_num_4x4_w = num_4x4_blocks_wide_lookup[m_mi_size];
        m_num_4x4_h = num_4x4_blocks_high_lookup[m_mi_size];
        for (auto idy = 0; idy < 2; idy += m_num_4x4_h) {
            for (auto idx = 0; idx < 2; idx += m_num_4x4_w) {
                auto inter_mode = m_tree_parser->parse_tree(SyntaxElementType::InterMode);
                m_y_mode = NearestMv + inter_mode;
                if (m_y_mode == NearestMv || m_y_mode == NearMv) {
                    for (auto j = 0; j < 1 + is_compound; j++)
                        SAFE_CALL(append_sub8x8_mvs(idy * 2 + idx, j));
                }
                SAFE_CALL(assign_mv(is_compound));
                for (auto y = 0; y < m_num_4x4_h; y++) {
                    for (auto x = 0; x < m_num_4x4_w; x++) {
                        auto block = (idy + y) * 2 + idx + x;
                        for (auto ref_list = 0; ref_list < 1 + is_compound; ref_list++) {
                            (void)block;
                            // TODO: m_block_mvs[ref_list][block] = m_mv[ref_list];
                        }
                    }
                }
            }
        }
        return true;
    }
    SAFE_CALL(assign_mv(is_compound));
    for (auto ref_list = 0; ref_list < 1 + is_compound; ref_list++) {
        for (auto block = 0; block < 4; block++) {
            // TODO: m_block_mvs[ref_list][block] = m_mv[ref_list];
        }
    }
    return true;
}

bool Decoder::read_ref_frames()
{
    if (seg_feature_active(SEG_LVL_REF_FRAME)) {
        m_ref_frame[0] = static_cast<ReferenceFrame>(m_feature_data[m_segment_id][SEG_LVL_REF_FRAME]);
        m_ref_frame[1] = None;
        return true;
    }
    ReferenceMode comp_mode;
    if (m_reference_mode == ReferenceModeSelect)
        comp_mode = m_tree_parser->parse_tree<ReferenceMode>(SyntaxElementType::CompMode);
    else
        comp_mode = m_reference_mode;
    if (comp_mode == CompoundReference) {
        auto idx = m_ref_frame_sign_bias[m_comp_fixed_ref];
        auto comp_ref = m_tree_parser->parse_tree(SyntaxElementType::CompRef);
        m_ref_frame[idx] = m_comp_fixed_ref;
        m_ref_frame[!idx] = m_comp_var_ref[comp_ref];
        return true;
    }
    auto single_ref_p1 = m_tree_parser->parse_tree<bool>(SyntaxElementType::SingleRefP1);
    if (single_ref_p1) {
        auto single_ref_p2 = m_tree_parser->parse_tree<bool>(SyntaxElementType::SingleRefP2);
        m_ref_frame[0] = single_ref_p2 ? AltRefFrame : GoldenFrame;
    } else {
        m_ref_frame[0] = LastFrame;
    }
    m_ref_frame[1] = None;
    return true;
}

bool Decoder::assign_mv(bool is_compound)
{
    m_mv[1] = ZeroMv;
    for (auto i = 0; i < 1 + is_compound; i++) {
        if (m_y_mode == NewMv) {
            SAFE_CALL(read_mv(i));
        } else if (m_y_mode == NearestMv) {
            m_mv[i] = m_nearest_mv[i];
        } else if (m_y_mode == NearMv) {
            m_mv[i] = m_near_mv[i];
        } else {
            m_mv[i] = ZeroMv;
        }
    }
    return true;
}

bool Decoder::read_mv(u8)
{
    // TODO: Implement
    return true;
}

bool Decoder::find_mv_refs(ReferenceFrame, int)
{
    // TODO: Implement
    return true;
}

bool Decoder::find_best_ref_mvs(int)
{
    // TODO: Implement
    return true;
}

bool Decoder::append_sub8x8_mvs(u8, u8)
{
    // TODO: Implement
    return true;
}

void Decoder::dump_info()
{
    dbgln("Frame dimensions: {}x{}", m_frame_width, m_frame_height);
    dbgln("Render dimensions: {}x{}", m_render_width, m_render_height);
    dbgln("Bit depth: {}", m_bit_depth);
    dbgln("Interpolation filter: {}", (u8)m_interpolation_filter);
}

Decoder::~Decoder()
{
    if (m_above_nonzero_context)
        free(m_above_nonzero_context);
    if (m_left_nonzero_context)
        free(m_left_nonzero_context);
    if (m_above_seg_pred_context)
        free(m_above_seg_pred_context);
    if (m_left_seg_pred_context)
        free(m_left_seg_pred_context);
    if (m_above_partition_context)
        free(m_above_partition_context);
    if (m_left_partition_context)
        free(m_left_partition_context);
}

}
