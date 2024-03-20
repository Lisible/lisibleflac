#include "lisibleflac.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LFLAC_ASSERT(x)                                                        \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "LFLAC Assertion failed:\n\t%s\n", #x);                  \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

#define LFLAC_LOG_PREFIX "LFLAC %s:%d: "
#define LFLAC_LOG0(msg)                                                        \
  fprintf(stderr, LFLAC_LOG_PREFIX msg "\n", __FILE__, __LINE__);
#define LFLAC_LOG(msg, ...)                                                    \
  fprintf(stderr, LFLAC_LOG_PREFIX msg "\n", __FILE__, __LINE__, __VA_ARGS__);

#define MIN(x, y) (((x) < (y)) ? (x) : (y))

typedef enum {
  FlacMetadataBlockType_StreamInfo = 0,
  FlacMetadataBlockType_Padding = 1,
  FlacMetadataBlockType_VorbisComment = 4
} FlacMetadataBlockType;

typedef struct {
  uint32_t length;
  uint8_t block_type;
  bool last;
} MetadataBlockHeader;

#define MD5_CHECKSUM_SIZE 16
typedef struct {
  uint8_t md5_checksum[MD5_CHECKSUM_SIZE];
  uint64_t sample_count;
  uint32_t sample_rate;
  uint32_t minimum_frame_size;
  uint32_t maximum_frame_size;
  uint16_t minimum_blocksize;
  uint16_t maximum_blocksize;
  uint8_t channel_count;
  uint8_t bits_per_sample;

} StreamInfo;

typedef struct {
  FILE *stream;
  uint8_t bit_index;
  uint8_t current_byte;
} FlacDecoder;

static bool FlacDecoder_init(FlacDecoder *decoder, FILE *stream) {
  LFLAC_ASSERT(decoder != NULL);
  LFLAC_ASSERT(stream != NULL);
  decoder->stream = stream;
  decoder->bit_index = 0;
  // Initializing the current_byte to the first byte of the stream
  return fread(&decoder->current_byte, 1, 1, stream) == 1;
}

static bool FlacDecoder_skip(FlacDecoder *decoder, size_t byte_count) {
  LFLAC_ASSERT(decoder != NULL);
  if (fseek(decoder->stream, byte_count - 1, SEEK_CUR) != 0) {
    return false;
  }

  return fread(&decoder->current_byte, 1, 1, decoder->stream);
}

static bool FlacDecoder_next_bits(FlacDecoder *decoder, uint32_t bit_count,
                                  uint32_t *bits) {
  LFLAC_ASSERT(decoder != NULL);
  LFLAC_ASSERT(bits != NULL);
  LFLAC_ASSERT(bit_count <= 32);

  uint32_t result = 0;
  if (decoder->bit_index > 0) {
    uint32_t left_in_byte = 8 - decoder->bit_index;
    uint32_t count = MIN(left_in_byte, bit_count);
    result = ((decoder->current_byte & ((1 << left_in_byte) - 1)) >>
              (left_in_byte - count));

    if (fread(&decoder->current_byte, 1, 1, decoder->stream) != 1) {
      return false;
    }
    decoder->bit_index = 0;
    bit_count -= count;
  }

  while (bit_count >= 8) {
    result = (result << 8) | (decoder->current_byte);
    bit_count -= 8;

    if (fread(&decoder->current_byte, 1, 1, decoder->stream) != 1) {
      return false;
    }
    decoder->bit_index = 0;
  }

  if (bit_count > 0) {
    uint32_t left_in_byte = 8 - decoder->bit_index;
    result = (result << bit_count) |
             ((decoder->current_byte & ((1 << left_in_byte) - 1)) >>
              (left_in_byte - bit_count));
    decoder->bit_index += bit_count;
  }

  *bits = result;
  return true;
}

static bool FlacDecoder_validate_signature(FlacDecoder *decoder) {
  LFLAC_ASSERT(decoder != NULL);
  uint32_t bits = 0;
  if (!FlacDecoder_next_bits(decoder, 32, &bits)) {
    return false;
  }

  return (bits & 0xFF) == 'C' && (bits >> 8 & 0xFF) == 'a' &&
         (bits >> 16 & 0xFF) == 'L' && (bits >> 24 & 0xFF) == 'f';
}

static bool FlacDecoder_parse_metadata_block_header(
    FlacDecoder *decoder, MetadataBlockHeader *output_metadata_block_header) {
  LFLAC_ASSERT(decoder != NULL);
  LFLAC_ASSERT(output_metadata_block_header != NULL);

  uint32_t block_info;
  if (!FlacDecoder_next_bits(decoder, 8, &block_info)) {
    return false;
  }
  output_metadata_block_header->last = block_info & 0x80;
  output_metadata_block_header->block_type = block_info & 0x7f;

  uint32_t length;
  if (!FlacDecoder_next_bits(decoder, 24, &length)) {
    return false;
  }

  output_metadata_block_header->length = length & ((1 << 24) - 1);
  return true;
}

static bool
FlacDecoder_parse_STREAMINFO_metadata_block(FlacDecoder *decoder,
                                            StreamInfo *output_stream_info) {
  LFLAC_ASSERT(decoder != NULL);
  LFLAC_ASSERT(output_stream_info != NULL);
  LFLAC_LOG0("Parse STREAMINFO metadata block");

  uint32_t minimum_blocksize;
  FlacDecoder_next_bits(decoder, 16, &minimum_blocksize);
  LFLAC_LOG("Minimum blocksize: %d samples", minimum_blocksize);

  uint32_t maximum_blocksize;
  FlacDecoder_next_bits(decoder, 16, &maximum_blocksize);
  LFLAC_LOG("Maximum blocksize: %d samples", maximum_blocksize);

  uint32_t minimum_frame_size;
  FlacDecoder_next_bits(decoder, 24, &minimum_frame_size);
  LFLAC_LOG("Minimum frame size: %d samples", minimum_frame_size);

  uint32_t maximum_frame_size;
  FlacDecoder_next_bits(decoder, 24, &maximum_frame_size);
  LFLAC_LOG("Maximum frame size: %d samples", maximum_frame_size);

  uint32_t sample_rate;
  FlacDecoder_next_bits(decoder, 20, &sample_rate);
  if (sample_rate == 0) {
    LFLAC_LOG0("Invalid sample rate");
    return false;
  }
  LFLAC_LOG("Sample rate: %d Hz", sample_rate);

  uint32_t channel_count_bps;
  FlacDecoder_next_bits(decoder, 8, &channel_count_bps);

  const uint8_t BPS_BIT_LENGTH = 5;
  uint8_t channel_count = (channel_count_bps >> BPS_BIT_LENGTH) + 1;
  LFLAC_LOG("Channel count: %d", channel_count);

  uint8_t bits_per_sample = (channel_count_bps & 0x1F) + 1;
  LFLAC_LOG("bits-per-sample: %d bits", bits_per_sample);

  uint32_t sample_count_hi;
  FlacDecoder_next_bits(decoder, 32, &sample_count_hi);
  uint32_t sample_count_lo;
  FlacDecoder_next_bits(decoder, 4, &sample_count_lo);
  uint64_t sample_count = (sample_count_hi << 4) | sample_count_lo;
  LFLAC_LOG("sample count: %zu", sample_count);

  uint8_t md5[MD5_CHECKSUM_SIZE];
  for (size_t i = 0; i < MD5_CHECKSUM_SIZE; i++) {
    uint32_t v;
    FlacDecoder_next_bits(decoder, 8, &v);
    md5[i] = (uint8_t)(v & 0xFF);
  }

  LFLAC_LOG0("md5:");
  for (size_t i = 0; i < MD5_CHECKSUM_SIZE; i++) {
    fprintf(stderr, "%x", md5[i]);
  }
  fprintf(stderr, "\n");

  output_stream_info->minimum_blocksize = minimum_blocksize;
  output_stream_info->maximum_blocksize = maximum_blocksize;
  output_stream_info->minimum_frame_size = minimum_frame_size;
  output_stream_info->maximum_frame_size = maximum_frame_size;
  output_stream_info->sample_rate = sample_rate;
  output_stream_info->channel_count = channel_count;
  output_stream_info->bits_per_sample = bits_per_sample;
  output_stream_info->sample_count = sample_count;
  memcpy(output_stream_info->md5_checksum, md5, MD5_CHECKSUM_SIZE);

  return true;
}

bool lflac_decode(FILE *stream) {
  LFLAC_ASSERT(stream != NULL);
  FlacDecoder decoder;
  FlacDecoder_init(&decoder, stream);
  if (!FlacDecoder_validate_signature(&decoder)) {
    LFLAC_LOG0("Invalid signature");
    return false;
  }

  StreamInfo stream_info;
  bool last_metadata_block = false;
  size_t block_count = 0;
  while (!last_metadata_block) {
    MetadataBlockHeader header;
    if (!FlacDecoder_parse_metadata_block_header(&decoder, &header)) {
      return false;
    }
    last_metadata_block = header.last;

    if (block_count == 0 &&
        header.block_type != FlacMetadataBlockType_StreamInfo) {
      LFLAC_LOG0("StreamInfo is not the first metadata block");
      return false;
    }

    bool could_parse_metadata_block = false;
    switch (header.block_type) {
    case FlacMetadataBlockType_StreamInfo:
      could_parse_metadata_block =
          FlacDecoder_parse_STREAMINFO_metadata_block(&decoder, &stream_info);
      break;
    case FlacMetadataBlockType_Padding:
    // We don't care about the vorbis comment block
    case FlacMetadataBlockType_VorbisComment:
      could_parse_metadata_block = FlacDecoder_skip(&decoder, header.length);
      break;
    default:
      LFLAC_LOG("Unsupported block type: %d", header.block_type);
      return false;
      break;
    }

    if (!could_parse_metadata_block) {
      LFLAC_LOG("Couldn't parse block of type %d", header.block_type);
      return false;
    }

    block_count++;
  }

  return true;
}

#undef MD5_CHECKSUM_SIZE
#undef LFLAC_LOG_PREFIX
#undef LFLAC_LOG0
#undef LFLAC_LOG
#undef LFLAC_ASSERT
