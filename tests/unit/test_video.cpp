/**
 * @file tests/unit/test_video.cpp
 * @brief Test src/video.*.
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// ffmpeg includes
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

// local includes
#include <src/config.h>
#include <src/video.h>

// ffmpeg includes
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
}

using namespace std::literals;

/**
 * @brief Packet replacement rules retain the original non-owning byte views.
 */
TEST(VideoPacketReplacementTest, CopiesViewsWithoutCopyingTheirBackingBytes) {
  std::string original = "__old__";
  std::string replacement = "__new__";
  const video::packet_raw_t::replace_t rule {std::string_view(original).substr(2, 3), std::string_view(replacement).substr(2, 3)};
  EXPECT_EQ(rule.old, "old");
  EXPECT_EQ(rule._new, "new");
  EXPECT_EQ(rule.old.data(), original.data() + 2);
  EXPECT_EQ(rule._new.data(), replacement.data() + 2);
  original[2] = 'O';
  replacement[2] = 'N';
  EXPECT_EQ(rule.old, "Old");
  EXPECT_EQ(rule._new, "New");
}

TEST(VideoFormatMappingTest, MapsSupportedHardwareDevicesAndRejectsUnsupportedOnes) {
  using enum platf::mem_type_e;
  const std::array cases {
    std::pair {AV_HWDEVICE_TYPE_D3D11VA, dxgi},
    std::pair {AV_HWDEVICE_TYPE_VAAPI, vaapi},
    std::pair {AV_HWDEVICE_TYPE_CUDA, cuda},
    std::pair {AV_HWDEVICE_TYPE_NONE, system},
    std::pair {AV_HWDEVICE_TYPE_VIDEOTOOLBOX, videotoolbox},
#ifdef SUNSHINE_BUILD_VULKAN
    std::pair {AV_HWDEVICE_TYPE_VULKAN, vulkan},
#else
    std::pair {AV_HWDEVICE_TYPE_VULKAN, unknown},
#endif
    std::pair {AV_HWDEVICE_TYPE_DXVA2, unknown},
  };

  for (const auto &[format, expected] : cases) {
    EXPECT_EQ(video::map_base_dev_type(format), expected) << format;
  }
}

TEST(VideoFormatMappingTest, MapsSupportedPixelFormatsAndRejectsUnsupportedOnes) {
  using enum platf::pix_fmt_e;
  const std::array cases {
    std::pair {AV_PIX_FMT_VUYX, ayuv},
    std::pair {AV_PIX_FMT_XV30, y410},
    std::pair {AV_PIX_FMT_YUV420P10, yuv420p10},
    std::pair {AV_PIX_FMT_YUV420P, yuv420p},
    std::pair {AV_PIX_FMT_NV12, nv12},
    std::pair {AV_PIX_FMT_P010, p010},
    std::pair {AV_PIX_FMT_YUV444P, yuv444p},
    std::pair {AV_PIX_FMT_YUV444P16, yuv444p16},
    std::pair {AV_PIX_FMT_NONE, unknown},
    std::pair {AV_PIX_FMT_RGB24, unknown},
  };

  for (const auto &[format, expected] : cases) {
    EXPECT_EQ(video::map_pix_fmt(format), expected) << format;
  }
}

struct EncoderTest: PlatformTestSuite, testing::WithParamInterface<video::encoder_t *> {
  void SetUp() override {
    BaseTest::SetUp();
    auto &encoder = *GetParam();
    if (!video::validate_encoder(encoder, false)) {
      // Encoder failed validation,
      // if it's software - fail, otherwise skip
      if (encoder.name == "software") {
        FAIL() << "Software encoder not available";
      } else {
        GTEST_SKIP() << "Encoder not available";
      }
    }
  }
};

/**
 * @brief Available encoder backends for this platform.
 */
const auto encoder_variants = std::to_array<video::encoder_t *>({
#if !defined(__APPLE__)
  &video::nvenc,
#endif
#ifdef _WIN32
  &video::amdvce,
  &video::quicksync,
#endif
#if defined(__linux__) || defined(__FreeBSD__)
  &video::vaapi,
#endif
#ifdef __APPLE__
  &video::videotoolbox,
#endif
  &video::software
});

INSTANTIATE_TEST_SUITE_P(
  EncoderVariants,
  EncoderTest,
  testing::ValuesIn(encoder_variants),
  [](const auto &info) {
    return std::string(info.param->name);
  }
);

#if !defined(_WIN32) && !defined(__APPLE__)
TEST(NvencAvcodecOptionsTest, UsesVersionStablePresetAndSpatialAqNames) {
  const std::array codecs {
    &video::nvenc.av1,
    &video::nvenc.hevc,
    &video::nvenc.h264,
  };

  for (const auto *codec : codecs) {
    const auto preset = std::ranges::find(codec->common_options, "preset"sv, &video::encoder_t::option_t::name);
    ASSERT_NE(codec->common_options.end(), preset);
    ASSERT_TRUE(std::holds_alternative<std::string *>(preset->value));
    EXPECT_EQ(&config::video.nv_legacy.preset, std::get<std::string *>(preset->value));

    const auto spatial_aq = std::ranges::find(codec->common_options, "spatial-aq"sv, &video::encoder_t::option_t::name);
    ASSERT_NE(codec->common_options.end(), spatial_aq);
    ASSERT_TRUE(std::holds_alternative<int *>(spatial_aq->value));
    EXPECT_EQ(&config::video.nv_legacy.spatial_aq, std::get<int *>(spatial_aq->value));

    EXPECT_EQ(
      codec->common_options.end(),
      std::ranges::find(codec->common_options, "aq"sv, &video::encoder_t::option_t::name)
    );
  }
}
#endif

TEST_P(EncoderTest, ValidateEncoder) {
  // todo:: test something besides fixture setup
}

/**
 * @brief Parameterized coverage for effective H.264 profile selection.
 */
struct H264ProfileTest: testing::TestWithParam<std::tuple<std::string_view, video::amf::coder_e, int, int>> {};

TEST_P(H264ProfileTest, SelectProfile) {
  const auto &[encoder_name, coder, chroma_sampling_type, expected_profile] = GetParam();
  video::config_t config {};
  config.chromaSamplingType = chroma_sampling_type;

  EXPECT_EQ(expected_profile, video::select_h264_profile(encoder_name, config, std::to_underlying(coder)));
}

INSTANTIATE_TEST_SUITE_P(
  H264ProfileTests,
  H264ProfileTest,
  testing::Values(
    std::make_tuple("h264_amf"sv, video::amf::coder_e::auto_, 0, AV_PROFILE_H264_HIGH),
    std::make_tuple("h264_amf"sv, video::amf::coder_e::cabac, 0, AV_PROFILE_H264_HIGH),
    std::make_tuple("h264_amf"sv, video::amf::coder_e::cavlc, 0, AV_PROFILE_H264_CONSTRAINED_BASELINE),
    std::make_tuple("h264_amf"sv, video::amf::coder_e::cavlc, 1, AV_PROFILE_H264_HIGH_444_PREDICTIVE),
    std::make_tuple("h264_nvenc"sv, video::amf::coder_e::cavlc, 0, AV_PROFILE_H264_HIGH)
  )
);

/**
 * @brief Parameterized coverage for resolving requested dynamic range against encoder capabilities.
 */
struct DynamicRangeTest: testing::TestWithParam<std::tuple<int, int, int, bool, bool, int>> {};

TEST_P(DynamicRangeTest, Resolve) {
  const auto &[video_format, chroma_sampling_type, requested_dynamic_range, supports_hdr, supports_hdr_yuv444, expected_dynamic_range] = GetParam();

  video::encoder_t encoder {
    "test"sv,
    {},
    {},
    {},
    {},
    0,
  };
  encoder.h264.name = "h264_test";
  encoder.hevc.name = "hevc_test";
  encoder.av1.name = "av1_test";

  video::config_t config {};
  config.videoFormat = video_format;
  config.dynamicRange = requested_dynamic_range;
  config.chromaSamplingType = chroma_sampling_type;

  auto *codec = &encoder.h264;
  if (video_format == 1) {
    codec = &encoder.hevc;
  } else if (video_format == 2) {
    codec = &encoder.av1;
  }
  (*codec)[video::encoder_t::DYNAMIC_RANGE] = supports_hdr;
  (*codec)[video::encoder_t::DYNAMIC_RANGE_YUV444] = supports_hdr_yuv444;

  const auto effective_config = video::resolve_dynamic_range(encoder, config);

  EXPECT_EQ(expected_dynamic_range, effective_config.dynamicRange);
  EXPECT_EQ(requested_dynamic_range, config.dynamicRange);
  EXPECT_EQ(video_format, effective_config.videoFormat);
  EXPECT_EQ(chroma_sampling_type, effective_config.chromaSamplingType);
}

INSTANTIATE_TEST_SUITE_P(
  DynamicRangeTests,
  DynamicRangeTest,
  testing::Values(
    std::make_tuple(0, 0, 1, false, true, 0),
    std::make_tuple(1, 0, 1, false, true, 0),
    std::make_tuple(1, 0, 1, true, false, 1),
    std::make_tuple(2, 1, 1, true, false, 0),
    std::make_tuple(2, 1, 1, false, true, 1),
    std::make_tuple(1, 0, 0, false, false, 0)
  )
);

/**
 * @brief Reset the published probe results before and after each test.
 */
struct EncoderProbeResultsTest: testing::Test {
  /**
   * @brief Start from an empty published state.
   */
  void SetUp() override {
    video::reset_encoder_probe_results();
  }

  /**
   * @brief Leave no published state behind for other tests.
   */
  void TearDown() override {
    video::reset_encoder_probe_results();
  }
};

TEST_F(EncoderProbeResultsTest, FailedCodecProbeDoesNotPublishCapabilities) {
  video::encoder_t encoder {"test"sv, {}, {}, {}, {}, 0};
  for (auto *codec : {&encoder.h264, &encoder.hevc, &encoder.av1}) {
    (*codec)[video::encoder_t::PASSED] = false;
    (*codec)[video::encoder_t::YUV444] = true;
    (*codec)[video::encoder_t::DYNAMIC_RANGE] = true;
    (*codec)[video::encoder_t::DYNAMIC_RANGE_YUV444] = true;
  }

  video::record_encoder_probe_results(encoder);

  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_FALSE(video::last_encoder_probe_supported_yuv444_for_codec[i]) << i;
    EXPECT_FALSE(video::last_encoder_probe_supported_dynamic_range_for_codec[i]) << i;
    EXPECT_FALSE(video::last_encoder_probe_supported_dynamic_range_yuv444_for_codec[i]) << i;
  }
}

TEST_F(EncoderProbeResultsTest, SubsequentProbeReplacesPriorResults) {
  constexpr uint32_t ref_frames_invalidation = 1 << 8;  // Mirrors REF_FRAMES_INVALIDATION in video.cpp.
  video::encoder_t first {"first"sv, {}, {}, {}, {}, ref_frames_invalidation};
  for (auto *codec : {&first.h264, &first.hevc, &first.av1}) {
    (*codec)[video::encoder_t::PASSED] = true;
    (*codec)[video::encoder_t::YUV444] = true;
    (*codec)[video::encoder_t::DYNAMIC_RANGE] = true;
    (*codec)[video::encoder_t::DYNAMIC_RANGE_YUV444] = true;
  }

  // The second encoder only supports plain 8-bit 4:2:0 H.264.
  video::encoder_t second {"second"sv, {}, {}, {}, {}, 0};
  second.h264[video::encoder_t::PASSED] = true;

  video::record_encoder_probe_results(first);
  ASSERT_TRUE(video::last_encoder_probe_supported_ref_frames_invalidation);
  ASSERT_TRUE(video::last_encoder_probe_supported_yuv444_for_codec[1]);
  ASSERT_TRUE(video::last_encoder_probe_supported_dynamic_range_for_codec[2]);
  ASSERT_TRUE(video::last_encoder_probe_supported_dynamic_range_yuv444_for_codec[2]);

  video::record_encoder_probe_results(second);

  EXPECT_FALSE(video::last_encoder_probe_supported_ref_frames_invalidation);
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_FALSE(video::last_encoder_probe_supported_yuv444_for_codec[i]) << i;
    EXPECT_FALSE(video::last_encoder_probe_supported_dynamic_range_for_codec[i]) << i;
    EXPECT_FALSE(video::last_encoder_probe_supported_dynamic_range_yuv444_for_codec[i]) << i;
  }
}

#ifdef _WIN32
TEST(AmfH264OptionsTest, CoderUsesConfiguredValue) {
  const auto coder_option = std::ranges::find(video::amdvce.h264.common_options, "coder"sv, &video::encoder_t::option_t::name);

  ASSERT_NE(video::amdvce.h264.common_options.end(), coder_option);
  ASSERT_TRUE(std::holds_alternative<int *>(coder_option->value));
  EXPECT_EQ(&config::video.amd.amd_coder, std::get<int *>(coder_option->value));
}

/**
 * @brief Parameterized coverage for the AMF maximum access-unit-size option mappings.
 */
struct AmfMaxAuSizeOptionsTest: testing::TestWithParam<std::tuple<const video::encoder_t::codec_t *, bool>> {};

TEST_P(AmfMaxAuSizeOptionsTest, UsesConfiguredValueForSupportedCodecsOnly) {
  const auto &[codec, supported] = GetParam();
  const auto option = std::ranges::find(codec->common_options, "max_au_size"sv, &video::encoder_t::option_t::name);

  if (!supported) {
    EXPECT_EQ(codec->common_options.end(), option);
    return;
  }

  ASSERT_NE(codec->common_options.end(), option);
  ASSERT_TRUE(std::holds_alternative<std::optional<int> *>(option->value));
  EXPECT_EQ(&config::video.amd.amd_max_au_size, std::get<std::optional<int> *>(option->value));
}

INSTANTIATE_TEST_SUITE_P(
  AmfCodecOptions,
  AmfMaxAuSizeOptionsTest,
  testing::Values(
    std::make_tuple(&video::amdvce.h264, true),
    std::make_tuple(&video::amdvce.hevc, true),
    std::make_tuple(&video::amdvce.av1, false)
  )
);
#endif

using AmfMaxAuSizeConfigParam = std::tuple<std::string_view, std::optional<int>>;

/**
 * @brief Parameterized coverage for parsing and validating the AMF maximum access-unit size.
 */
struct AmfMaxAuSizeConfigTest: BaseTest, testing::WithParamInterface<AmfMaxAuSizeConfigParam> {
  void SetUp() override {
    BaseTest::SetUp();
    config::video.amd.amd_max_au_size.reset();
    config::stream.file_apps = SUNSHINE_SOURCE_DIR "/tests/unit/test_video.cpp";
  }

  void TearDown() override {
    config::video = original_video;
    config::audio = original_audio;
    config::stream = original_stream;
    config::nvhttp = original_nvhttp;
    config::input = original_input;
    config::sunshine = original_sunshine;
    config::modified_config_settings = original_modified_config_settings;
    BaseTest::TearDown();
  }

  config::video_t original_video {config::video};  ///< Video configuration restored after each parameterized test.
  config::audio_t original_audio {config::audio};  ///< Audio configuration restored after each parameterized test.
  config::stream_t original_stream {config::stream};  ///< Stream configuration restored after each parameterized test.
  config::nvhttp_t original_nvhttp {config::nvhttp};  ///< HTTP configuration restored after each parameterized test.
  config::input_t original_input {config::input};  ///< Input configuration restored after each parameterized test.
  config::sunshine_t original_sunshine {config::sunshine};  ///< Core configuration restored after each parameterized test.
  decltype(config::modified_config_settings) original_modified_config_settings {config::modified_config_settings};  ///< Modified settings restored after each parameterized test.
};

TEST_P(AmfMaxAuSizeConfigTest, AcceptsOnlyFfmpegSupportedRange) {
  const auto &[setting, expected] = GetParam();
  config::apply_config_for_test(setting);

  EXPECT_EQ(expected, config::video.amd.amd_max_au_size);
}

INSTANTIATE_TEST_SUITE_P(
  AmfMaxAuSizeValues,
  AmfMaxAuSizeConfigTest,
  testing::Values(
    AmfMaxAuSizeConfigParam {""sv, std::nullopt},
    AmfMaxAuSizeConfigParam {"amd_max_au_size = -2\n"sv, std::nullopt},
    AmfMaxAuSizeConfigParam {"amd_max_au_size = -1\n"sv, -1},
    AmfMaxAuSizeConfigParam {"amd_max_au_size = 0\n"sv, 0},
    AmfMaxAuSizeConfigParam {"amd_max_au_size = 800000\n"sv, 800000},
    AmfMaxAuSizeConfigParam {"amd_max_au_size = 2147483647\n"sv, std::numeric_limits<int>::max()}
  )
);

struct FramerateX100Test: BaseTest, testing::WithParamInterface<std::tuple<std::int32_t, AVRational>> {};

TEST_P(FramerateX100Test, Run) {
  const auto &[x100, expected] = GetParam();
  auto res = video::framerateX100_to_rational(x100);
  ASSERT_EQ(0, av_cmp_q(res, expected)) << "expected "
                                        << expected.num << "/" << expected.den
                                        << ", got "
                                        << res.num << "/" << res.den;
}

INSTANTIATE_TEST_SUITE_P(
  FramerateX100Tests,
  FramerateX100Test,
  testing::Values(
    std::make_tuple(2397, AVRational {24000, 1001}),
    std::make_tuple(2398, AVRational {24000, 1001}),
    std::make_tuple(2500, AVRational {25, 1}),
    std::make_tuple(2997, AVRational {30000, 1001}),
    std::make_tuple(3000, AVRational {30, 1}),
    std::make_tuple(5994, AVRational {60000, 1001}),
    std::make_tuple(6000, AVRational {60, 1}),
    std::make_tuple(11988, AVRational {120000, 1001}),
    std::make_tuple(23976, AVRational {240000, 1001}),  // future NTSC 240hz?
    std::make_tuple(9498, AVRational {4749, 50})  // from my LG 27GN950
  )
);

struct FramerateToRationalTest: testing::TestWithParam<std::tuple<int, int, AVRational>> {};

TEST_P(FramerateToRationalTest, Run) {
  const auto &[framerate, framerateX100, expected] = GetParam();
  video::config_t config {};
  config.framerate = framerate;
  config.framerateX100 = framerateX100;
  auto res = video::framerate_to_rational(config);
  ASSERT_EQ(0, av_cmp_q(res, expected)) << "expected "
                                        << expected.num << "/" << expected.den
                                        << ", got "
                                        << res.num << "/" << res.den;
}

INSTANTIATE_TEST_SUITE_P(
  FramerateToRationalTests,
  FramerateToRationalTest,
  testing::Values(
    std::make_tuple(60, 0, AVRational {60, 1}),  // no X100 value, fall back to integer framerate
    std::make_tuple(60, 5994, AVRational {60000, 1001}),
    std::make_tuple(120, 11988, AVRational {120000, 1001}),
    std::make_tuple(24, 2398, AVRational {24000, 1001})
  )
);

struct CaptureFrameIntervalTest: testing::TestWithParam<std::tuple<int, int, std::chrono::nanoseconds>> {};

TEST_P(CaptureFrameIntervalTest, Run) {
  const auto &[framerate, framerateX100, expected] = GetParam();
  video::config_t config {};
  config.framerate = framerate;
  config.framerateX100 = framerateX100;
  ASSERT_EQ(expected, video::capture_frame_interval(config));
}

INSTANTIATE_TEST_SUITE_P(
  CaptureFrameIntervalTests,
  CaptureFrameIntervalTest,
  testing::Values(
    std::make_tuple(60, 0, std::chrono::nanoseconds {16666666}),
    std::make_tuple(60, 5994, std::chrono::nanoseconds {16683333}),  // 1e9 * 1001 / 60000
    std::make_tuple(120, 11988, std::chrono::nanoseconds {8341666})  // 1e9 * 1001 / 120000
  )
);

/**
 * @brief Software encoder converts BGR0 and NV12 frames, including padded strides and
 *        backends that don't report the pixel pitch.
 */
TEST(SoftwareEncoderConversion, Bgr0AndNv12) {
  constexpr int w = 320;
  constexpr int h = 240;

  AVFrame *frame = av_frame_alloc();
  ASSERT_NE(frame, nullptr);
  frame->width = w;
  frame->height = h;
  frame->format = AV_PIX_FMT_YUV420P;

  video::avcodec_software_encode_device_t device;
  ASSERT_EQ(device.init(w, h, frame, AV_PIX_FMT_YUV420P, false), 0);
  // set_frame() takes ownership of the frame; the device frees it on destruction.
  ASSERT_EQ(device.set_frame(frame, nullptr), 0);

  // BGR0 frame (4 bytes per pixel) -- the classic KMS/DMABUF capture layout.
  std::vector<uint8_t> bgr0_buffer(static_cast<size_t>(w) * h * 4);
  platf::img_t bgr0_img {};
  bgr0_img.data = bgr0_buffer.data();
  bgr0_img.width = w;
  bgr0_img.height = h;
  bgr0_img.row_pitch = w * 4;
  bgr0_img.pixel_pitch = 4;
  EXPECT_EQ(device.convert(bgr0_img), 0);

  // NV12 frame (1 byte per pixel row pitch, Y plane + interleaved UV) -- the
  // layout delivered by PipeWire-based captures (KWin screencast / portal).
  std::vector<uint8_t> nv12_buffer(static_cast<size_t>(w) * h + static_cast<size_t>(w) * h / 2);
  platf::img_t nv12_img {};
  nv12_img.data = nv12_buffer.data();
  nv12_img.width = w;
  nv12_img.height = h;
  nv12_img.row_pitch = w;
  nv12_img.pixel_pitch = 1;
  EXPECT_EQ(device.convert(nv12_img), 0);

  // Padded-stride NV12 (alignment padding) -- the case the old row_pitch ==
  // width heuristic misdetected as BGR0, causing out-of-bounds reads.
  constexpr int padded_stride = w + 32;
  std::vector<uint8_t> padded_nv12_buffer(static_cast<size_t>(padded_stride) * h + static_cast<size_t>(padded_stride) * h / 2);
  platf::img_t padded_nv12_img {};
  padded_nv12_img.data = padded_nv12_buffer.data();
  padded_nv12_img.width = w;
  padded_nv12_img.height = h;
  padded_nv12_img.row_pitch = padded_stride;
  padded_nv12_img.pixel_pitch = 1;
  EXPECT_EQ(device.convert(padded_nv12_img), 0);

  // Capture backends that don't report pixel_pitch fall back to deriving it
  // from the row pitch (1 byte per pixel = NV12, 4 = BGR0).
  platf::img_t fallback_bgr0_img {};
  fallback_bgr0_img.data = bgr0_buffer.data();
  fallback_bgr0_img.width = w;
  fallback_bgr0_img.height = h;
  fallback_bgr0_img.row_pitch = w * 4;
  EXPECT_EQ(device.convert(fallback_bgr0_img), 0);

  platf::img_t fallback_nv12_img {};
  fallback_nv12_img.data = nv12_buffer.data();
  fallback_nv12_img.width = w;
  fallback_nv12_img.height = h;
  fallback_nv12_img.row_pitch = w;
  EXPECT_EQ(device.convert(fallback_nv12_img), 0);
}

/**
 * @brief Restores the global video configuration and probe modes that the advertised codec tests change.
 */
class VideoAdvertisedCodecTest: public BaseTest {
protected:
  /**
   * @brief Save the global state and start with only H.264 able to be advertised.
   */
  void SetUp() override {
    BaseTest::SetUp();
    h264_mode_ = config::video.h264_mode;
    hevc_mode_ = video::active_hevc_mode;
    av1_mode_ = video::active_av1_mode;
    h263_mode_ = video::active_h263_mode;
    mpeg2_mode_ = video::active_mpeg2_mode;

    video::active_hevc_mode = 1;
    video::active_av1_mode = 1;
    video::active_h263_mode = 0;
    video::active_mpeg2_mode = 0;
  }

  /**
   * @brief Restore the global state.
   */
  void TearDown() override {
    config::video.h264_mode = h264_mode_;
    video::active_hevc_mode = hevc_mode_;
    video::active_av1_mode = av1_mode_;
    video::active_h263_mode = h263_mode_;
    video::active_mpeg2_mode = mpeg2_mode_;
    BaseTest::TearDown();
  }

private:
  int h264_mode_ = 0;  ///< Saved H.264 setting.
  int hevc_mode_ = 0;  ///< Saved HEVC mode.
  int av1_mode_ = 0;  ///< Saved AV1 mode.
  int h263_mode_ = 0;  ///< Saved H.263+ mode.
  int mpeg2_mode_ = 0;  ///< Saved MPEG-2 mode.
};

TEST_F(VideoAdvertisedCodecTest, H264IsEnabledUnlessTheSettingTurnsItOff) {
  config::video.h264_mode = 0;
  EXPECT_TRUE(video::h264_enabled());

  config::video.h264_mode = 1;
  EXPECT_FALSE(video::h264_enabled());
}

TEST_F(VideoAdvertisedCodecTest, EnabledH264AlwaysCountsAsAnAdvertisedCodec) {
  config::video.h264_mode = 0;
  EXPECT_TRUE(video::has_advertised_codec());
}

TEST_F(VideoAdvertisedCodecTest, DisabledH264WithNoOtherCodecAdvertisesNothing) {
  config::video.h264_mode = 1;
  EXPECT_FALSE(video::has_advertised_codec());
}

TEST_F(VideoAdvertisedCodecTest, DisabledH264NeedsAnotherActiveCodecToStillAdvertiseSomething) {
  config::video.h264_mode = 1;

  video::active_h263_mode = 1;
  EXPECT_TRUE(video::has_advertised_codec());
  video::active_h263_mode = 0;

  video::active_mpeg2_mode = 1;
  EXPECT_TRUE(video::has_advertised_codec());
  video::active_mpeg2_mode = 0;

  video::active_hevc_mode = 2;
  EXPECT_TRUE(video::has_advertised_codec());
  video::active_hevc_mode = 1;

  video::active_av1_mode = 2;
  EXPECT_TRUE(video::has_advertised_codec());
}

TEST(VideoH263Test, CodecFromConfigSelectsTheRequestedCodec) {
  video::encoder_t encoder {"test"sv, {}, {}, {}, {}, 0};
  encoder.h263.name = "h263p"s;

  video::config_t config {};
  config.videoFormat = 0;
  EXPECT_EQ(&encoder.h264, &encoder.codec_from_config(config));
  config.videoFormat = 1;
  EXPECT_EQ(&encoder.hevc, &encoder.codec_from_config(config));
  config.videoFormat = 2;
  EXPECT_EQ(&encoder.av1, &encoder.codec_from_config(config));
  config.videoFormat = 3;
  EXPECT_EQ(&encoder.h263, &encoder.codec_from_config(config));
}

TEST(VideoH263Test, OnlyTheSoftwareEncoderProducesH263) {
  EXPECT_EQ("h263p"sv, video::software.h263.name);
  EXPECT_FALSE(video::software.h263[video::encoder_t::PASSED]);

  const auto strict = std::ranges::find(video::software.h263.common_options, "strict"sv, &video::encoder_t::option_t::name);
  ASSERT_NE(video::software.h263.common_options.end(), strict);
  ASSERT_TRUE(std::holds_alternative<std::string>(strict->value));
  EXPECT_EQ("unofficial"s, std::get<std::string>(strict->value));

  const auto sc_threshold = std::ranges::find(video::software.h263.common_options, "sc_threshold"sv, &video::encoder_t::option_t::name);
  ASSERT_NE(video::software.h263.common_options.end(), sc_threshold);
  ASSERT_TRUE(std::holds_alternative<int>(sc_threshold->value));
  EXPECT_EQ(1000000000, std::get<int>(sc_threshold->value));

#ifndef __APPLE__
  EXPECT_TRUE(video::nvenc.h263.name.empty());
  EXPECT_FALSE(video::nvenc.h263[video::encoder_t::PASSED]);
#endif
}

/**
 * @brief Parameterized coverage for the stream sizes FFmpeg's H.263 encoder accepts.
 */
struct H263DimensionsTest: testing::TestWithParam<std::tuple<int, int, bool>> {};

TEST_P(H263DimensionsTest, Check) {
  const auto &[width, height, expected] = GetParam();
  EXPECT_EQ(expected, video::h263_dimensions_supported(width, height)) << width << 'x' << height;
}

INSTANTIATE_TEST_SUITE_P(
  H263DimensionsTests,
  H263DimensionsTest,
  testing::Values(
    std::make_tuple(400, 240, true),
    std::make_tuple(320, 240, true),
    std::make_tuple(800, 480, true),
    std::make_tuple(640, 360, true),
    std::make_tuple(1920, 1080, true),
    std::make_tuple(2048, 1152, true),
    std::make_tuple(4, 4, true),
    std::make_tuple(398, 240, false),
    std::make_tuple(400, 242, false),
    std::make_tuple(2052, 1080, false),
    std::make_tuple(2048, 1156, false),
    std::make_tuple(1152, 2048, false),
    std::make_tuple(0, 240, false),
    std::make_tuple(400, 0, false),
    std::make_tuple(-400, 240, false),
    std::make_tuple(400, -240, false)
  )
);

/**
 * @brief Parameterized coverage for the VBV buffer size given to the H.263 encoder.
 */
struct MpegRcBufferSizeTest: testing::TestWithParam<std::tuple<int, int, int, int>> {};

TEST_P(MpegRcBufferSizeTest, RoundsUpToAtLeastOneFrame) {
  const auto &[bitrate, fps_num, fps_den, expected] = GetParam();
  const AVRational fps {fps_num, fps_den};
  const auto size = video::mpeg_rc_buffer_size(bitrate, fps);
  EXPECT_EQ(expected, size);

  // FFmpeg's check is bit_rate * time_base.num > rc_buffer_size * time_base.den, with time_base = 1 / fps
  EXPECT_LE(static_cast<int64_t>(bitrate) * fps_den, static_cast<int64_t>(size) * fps_num);
}

INSTANTIATE_TEST_SUITE_P(
  MpegRcBufferSizeTests,
  MpegRcBufferSizeTest,
  testing::Values(
    std::make_tuple(3000000, 30, 1, 100000),
    std::make_tuple(5000000, 60, 1, 83334),
    std::make_tuple(5000000, 60000, 1001, 83417),
    std::make_tuple(10000000, 120, 1, 83334)
  )
);

TEST(VideoH263Test, EncoderOpensWithTheSoftwareOptionSetAndProducesKeyedPictures) {
  const auto *codec = avcodec_find_encoder_by_name(video::software.h263.name.c_str());
  if (!codec) {
    GTEST_SKIP() << "this FFmpeg build has no h263p encoder";
  }

  constexpr int width = 400;
  constexpr int height = 240;
  constexpr int bitrate = 5000 * 1000;
  const AVRational fps {60, 1};

  std::unique_ptr<AVCodecContext, void (*)(AVCodecContext *)> ctx {
    avcodec_alloc_context3(codec),
    [](AVCodecContext *c) {
      avcodec_free_context(&c);
    }
  };
  ASSERT_NE(nullptr, ctx);

  // The settings make_avcodec_encode_session() applies to every software encode
  ctx->width = width;
  ctx->height = height;
  ctx->framerate = fps;
  ctx->time_base = AVRational {fps.den, fps.num};
  ctx->pix_fmt = AV_PIX_FMT_YUV420P;
  ctx->max_b_frames = 0;
  ctx->gop_size = 600;
  ctx->keyint_min = std::numeric_limits<int>::max();
  ctx->flags = AV_CODEC_FLAG_CLOSED_GOP | AV_CODEC_FLAG_LOW_DELAY;
  ctx->flags2 |= AV_CODEC_FLAG2_FAST;
  ctx->rc_max_rate = bitrate;
  ctx->bit_rate = bitrate;
  ctx->rc_min_rate = bitrate;
  ctx->rc_buffer_size = video::mpeg_rc_buffer_size(bitrate, fps);
  ctx->slices = 2;
  ctx->thread_type = FF_THREAD_SLICE;
  ctx->thread_count = ctx->slices;

  AVDictionary *options = nullptr;
  for (const auto &option : video::software.h263.common_options) {
    if (const auto *text = std::get_if<std::string>(&option.value)) {
      av_dict_set(&options, option.name.c_str(), text->c_str(), 0);
    } else if (const auto *number = std::get_if<int>(&option.value)) {
      av_dict_set_int(&options, option.name.c_str(), *number, 0);
    }
  }
  ASSERT_GE(avcodec_open2(ctx.get(), codec, &options), 0);
  av_dict_free(&options);

  std::unique_ptr<AVFrame, void (*)(AVFrame *)> frame {
    av_frame_alloc(),
    [](AVFrame *f) {
      av_frame_free(&f);
    }
  };
  frame->format = AV_PIX_FMT_YUV420P;
  frame->width = width;
  frame->height = height;
  ASSERT_GE(av_frame_get_buffer(frame.get(), 0), 0);

  std::unique_ptr<AVPacket, void (*)(AVPacket *)> packet {
    av_packet_alloc(),
    [](AVPacket *p) {
      av_packet_free(&p);
    }
  };

  int keyed = 0;
  int received = 0;
  for (int i = 0; i < 4; ++i) {
    ASSERT_GE(av_frame_make_writable(frame.get()), 0);
    for (int plane = 0; plane < 3; ++plane) {
      const auto rows = plane == 0 ? height : height / 2;
      std::fill_n(frame->data[plane], static_cast<std::size_t>(frame->linesize[plane]) * rows, static_cast<uint8_t>(64 + 40 * plane + i));
    }
    frame->pts = i;

    // Sunshine requests an IDR frame by forcing the picture type, like encode() in video.cpp does
    const bool idr = i == 0 || i == 2;
    frame->pict_type = idr ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (idr) {
      frame->flags |= AV_FRAME_FLAG_KEY;
    } else {
      frame->flags &= ~AV_FRAME_FLAG_KEY;
    }

    ASSERT_GE(avcodec_send_frame(ctx.get(), frame.get()), 0);
    while (avcodec_receive_packet(ctx.get(), packet.get()) == 0) {
      ++received;
      ASSERT_GE(packet->size, 4);

      // Every picture starts with the byte aligned H.263 Picture Start Code
      EXPECT_EQ(0x00, packet->data[0]);
      EXPECT_EQ(0x00, packet->data[1]);
      EXPECT_EQ(0x80, packet->data[2] & 0xFC);

      const bool key = packet->flags & AV_PKT_FLAG_KEY;
      EXPECT_EQ(idr, key) << "picture " << i;
      keyed += key;
      av_packet_unref(packet.get());
    }
  }
  EXPECT_EQ(4, received);
  EXPECT_EQ(2, keyed);
}

TEST(VideoMpeg2Test, CodecFromConfigSelectsMpeg2ForFormatFour) {
  video::encoder_t encoder {"test"sv, {}, {}, {}, {}, 0};
  encoder.mpeg2.name = "mpeg2video"s;

  video::config_t config {};
  config.videoFormat = 4;
  EXPECT_EQ(&encoder.mpeg2, &encoder.codec_from_config(config));
  config.videoFormat = 3;
  EXPECT_EQ(&encoder.h263, &encoder.codec_from_config(config));
}

TEST(VideoMpeg2Test, OnlyTheSoftwareEncoderProducesMpeg2) {
  EXPECT_EQ("mpeg2video"sv, video::software.mpeg2.name);
  EXPECT_FALSE(video::software.mpeg2[video::encoder_t::PASSED]);

  const auto sc_threshold = std::ranges::find(video::software.mpeg2.common_options, "sc_threshold"sv, &video::encoder_t::option_t::name);
  ASSERT_NE(video::software.mpeg2.common_options.end(), sc_threshold);
  ASSERT_TRUE(std::holds_alternative<int>(sc_threshold->value));
  EXPECT_EQ(1000000000, std::get<int>(sc_threshold->value));

  // MPEG-2 needs no relaxed compliance, unlike H.263+
  EXPECT_EQ(video::software.mpeg2.common_options.end(), std::ranges::find(video::software.mpeg2.common_options, "strict"sv, &video::encoder_t::option_t::name));

#ifndef __APPLE__
  EXPECT_TRUE(video::nvenc.mpeg2.name.empty());
  EXPECT_FALSE(video::nvenc.mpeg2[video::encoder_t::PASSED]);
#endif
}

/**
 * @brief Parameterized coverage for the stream sizes FFmpeg's MPEG-2 encoder accepts.
 */
struct Mpeg2DimensionsTest: testing::TestWithParam<std::tuple<int, int, bool>> {};

TEST_P(Mpeg2DimensionsTest, Check) {
  const auto &[width, height, expected] = GetParam();
  EXPECT_EQ(expected, video::mpeg2_dimensions_supported(width, height)) << width << 'x' << height;
}

INSTANTIATE_TEST_SUITE_P(
  Mpeg2DimensionsTests,
  Mpeg2DimensionsTest,
  testing::Values(
    std::make_tuple(400, 240, true),
    std::make_tuple(320, 240, true),
    std::make_tuple(800, 480, true),
    std::make_tuple(1920, 1080, true),
    std::make_tuple(3840, 2160, true),
    std::make_tuple(398, 240, true),
    std::make_tuple(399, 239, true),
    std::make_tuple(2, 2, true),
    std::make_tuple(4095, 64, true),
    std::make_tuple(4098, 64, true),
    std::make_tuple(8191, 16383, true),
    std::make_tuple(16383, 16383, true),
    std::make_tuple(64, 16383, true),
    std::make_tuple(4096, 240, false),
    std::make_tuple(240, 4096, false),
    std::make_tuple(4097, 240, false),
    std::make_tuple(240, 4097, false),
    std::make_tuple(8192, 240, false),
    std::make_tuple(8193, 240, false),
    std::make_tuple(12288, 240, false),
    std::make_tuple(12289, 240, false),
    std::make_tuple(16384, 240, false),
    std::make_tuple(240, 16384, false),
    std::make_tuple(1, 240, false),
    std::make_tuple(240, 1, false),
    std::make_tuple(0, 240, false),
    std::make_tuple(240, 0, false),
    std::make_tuple(-400, 240, false),
    std::make_tuple(240, -400, false)
  )
);

/**
 * @brief Parameterized coverage for the framerates FFmpeg's MPEG-2 encoder accepts, as numerator, denominator and result.
 */
struct Mpeg2FramerateTest: testing::TestWithParam<std::tuple<int, int, bool>> {};

TEST_P(Mpeg2FramerateTest, Check) {
  const auto &[num, den, expected] = GetParam();
  EXPECT_EQ(expected, video::mpeg2_framerate_supported(AVRational {num, den})) << num << '/' << den;
}

INSTANTIATE_TEST_SUITE_P(
  Mpeg2FramerateTests,
  Mpeg2FramerateTest,
  testing::Values(
    std::make_tuple(24, 1, true),
    std::make_tuple(25, 1, true),
    std::make_tuple(30, 1, true),
    std::make_tuple(50, 1, true),
    std::make_tuple(60, 1, true),
    std::make_tuple(72, 1, true),
    std::make_tuple(75, 1, true),
    std::make_tuple(90, 1, true),
    std::make_tuple(100, 1, true),
    std::make_tuple(120, 1, true),
    std::make_tuple(180, 1, true),
    std::make_tuple(240, 1, true),
    std::make_tuple(24000, 1001, true),
    std::make_tuple(30000, 1001, true),
    std::make_tuple(60000, 1001, true),
    std::make_tuple(90000, 1001, true),
    std::make_tuple(120000, 1001, true),
    std::make_tuple(7, 1, false),
    std::make_tuple(143, 1, false),
    std::make_tuple(144, 1, false),
    std::make_tuple(300, 1, false),
    std::make_tuple(144000, 1001, false),
    std::make_tuple(0, 1, false),
    std::make_tuple(60, 0, false),
    std::make_tuple(-60, 1, false)
  )
);

TEST(VideoMpeg2Test, EncoderOpensWithTheSoftwareOptionSetAndProducesKeyedPictures) {
  const auto *codec = avcodec_find_encoder_by_name(video::software.mpeg2.name.c_str());
  if (!codec) {
    GTEST_SKIP() << "this FFmpeg build has no mpeg2video encoder";
  }

  constexpr int width = 400;
  constexpr int height = 240;
  constexpr int bitrate = 5000 * 1000;
  const AVRational fps {60, 1};

  std::unique_ptr<AVCodecContext, void (*)(AVCodecContext *)> ctx {
    avcodec_alloc_context3(codec),
    [](AVCodecContext *c) {
      avcodec_free_context(&c);
    }
  };
  ASSERT_NE(nullptr, ctx);

  // The settings make_avcodec_encode_session() applies to every software encode
  ctx->width = width;
  ctx->height = height;
  ctx->framerate = fps;
  ctx->time_base = AVRational {fps.den, fps.num};
  ctx->pix_fmt = AV_PIX_FMT_YUV420P;
  ctx->max_b_frames = 0;
  ctx->gop_size = 600;
  ctx->keyint_min = std::numeric_limits<int>::max();
  ctx->flags = AV_CODEC_FLAG_CLOSED_GOP | AV_CODEC_FLAG_LOW_DELAY;
  ctx->flags2 |= AV_CODEC_FLAG2_FAST;
  ctx->rc_max_rate = bitrate;
  ctx->bit_rate = bitrate;
  ctx->rc_min_rate = bitrate;
  ctx->rc_buffer_size = video::mpeg_rc_buffer_size(bitrate, fps);
  ctx->slices = 2;
  ctx->thread_type = FF_THREAD_SLICE;
  ctx->thread_count = ctx->slices;

  AVDictionary *options = nullptr;
  for (const auto &option : video::software.mpeg2.common_options) {
    if (const auto *text = std::get_if<std::string>(&option.value)) {
      av_dict_set(&options, option.name.c_str(), text->c_str(), 0);
    } else if (const auto *number = std::get_if<int>(&option.value)) {
      av_dict_set_int(&options, option.name.c_str(), *number, 0);
    }
  }
  ASSERT_GE(avcodec_open2(ctx.get(), codec, &options), 0);
  av_dict_free(&options);

  std::unique_ptr<AVFrame, void (*)(AVFrame *)> frame {
    av_frame_alloc(),
    [](AVFrame *f) {
      av_frame_free(&f);
    }
  };
  frame->format = AV_PIX_FMT_YUV420P;
  frame->width = width;
  frame->height = height;
  ASSERT_GE(av_frame_get_buffer(frame.get(), 0), 0);

  std::unique_ptr<AVPacket, void (*)(AVPacket *)> packet {
    av_packet_alloc(),
    [](AVPacket *p) {
      av_packet_free(&p);
    }
  };

  std::vector<bool> keys;
  const auto drain = [&]() {
    while (avcodec_receive_packet(ctx.get(), packet.get()) == 0) {
      EXPECT_GE(packet->size, 4);
      if (packet->size >= 4) {
        // Every picture starts with a start code prefix, followed by a sequence header when it is keyed
        // and by a picture header when it is not
        const bool key = packet->flags & AV_PKT_FLAG_KEY;
        EXPECT_EQ(0x00, packet->data[0]);
        EXPECT_EQ(0x00, packet->data[1]);
        EXPECT_EQ(0x01, packet->data[2]);
        EXPECT_EQ(key ? 0xB3 : 0x00, packet->data[3]);
        keys.push_back(key);
      }
      av_packet_unref(packet.get());
    }
  };

  for (int i = 0; i < 4; ++i) {
    ASSERT_GE(av_frame_make_writable(frame.get()), 0);
    for (int plane = 0; plane < 3; ++plane) {
      const auto rows = plane == 0 ? height : height / 2;
      std::fill_n(frame->data[plane], static_cast<std::size_t>(frame->linesize[plane]) * rows, static_cast<uint8_t>(64 + 40 * plane + i));
    }
    frame->pts = i;

    // Sunshine requests an IDR frame by forcing the picture type, like encode() in video.cpp does
    const bool idr = i == 0 || i == 2;
    frame->pict_type = idr ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (idr) {
      frame->flags |= AV_FRAME_FLAG_KEY;
    } else {
      frame->flags &= ~AV_FRAME_FLAG_KEY;
    }

    ASSERT_GE(avcodec_send_frame(ctx.get(), frame.get()), 0);
    drain();
  }

  ASSERT_GE(avcodec_send_frame(ctx.get(), nullptr), 0);
  drain();

  EXPECT_EQ((std::vector<bool> {true, false, true, false}), keys);
}
