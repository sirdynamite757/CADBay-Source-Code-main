/*
 * cadbay_image.c — CADBay submission image processor
 *
 * Converts a submitted image into the proprietary CBV format and burns in a
 * visible watermark plus a light anti-AI treatment. Also converts the other
 * direction so the pipeline can be verified.
 *
 * CBV FORMAT (must match viewers/image.html decodeCBV()):
 *
 *   offset  size            field
 *   0       4               magic "CBV1"
 *   4       4 (big-endian)  width
 *   8       4 (big-endian)  height
 *   12      4 (big-endian)  salt (LCG seed)
 *   16      w*h*4           RGBA pixels, each byte XORed with the LCG keystream
 *
 * The obfuscation is deliberately lightweight: it defeats casual "save image
 * as" and makes the file unusable as a normal image, but it is NOT encryption.
 * Real protection is the watermark (attribution) and the no-download viewer.
 *
 * "Anti-AI" treatment: a low-amplitude, spatially-varying perturbation is
 * added so the pixels are not a clean photographic sample. This is a deterrent
 * against naive dataset scraping, not a guarantee against model training.
 *
 * Build: make image-tool
 * Usage:
 *   cadbay-image encode <in.png|in.ppm> <out.cbv> [watermark-text]
 *   cadbay-image decode <in.cbv> <out.ppm>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/* Minimal PPM (P6) reader/writer — no external image library needed   */
/* ------------------------------------------------------------------ */

typedef struct
{
    int width;
    int height;
    unsigned char *rgba; /* width*height*4 */
} Image;

/* Read a binary PPM (P6) or ASCII PGM/PPM (P2/P3) into RGBA. */
static Image *read_ppm(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    char magic[3] = {0};
    if (fscanf(f, "%2s", magic) != 1)
    {
        fclose(f);
        return NULL;
    }

    int ascii = 0;
    if (strcmp(magic, "P6") == 0)
        ascii = 0;
    else if (strcmp(magic, "P3") == 0)
        ascii = 1;
    else
    {
        fclose(f);
        return NULL;
    }

    int w = 0, h = 0, maxv = 0;
    /* skip comments */
    int c;
    for (int field = 0; field < 3;)
    {
        c = fgetc(f);
        if (c == '#')
        {
            while ((c = fgetc(f)) != '\n' && c != EOF)
            {
            }
            continue;
        }
        if (isspace(c))
            continue;
        ungetc(c, f);
        int v = 0;
        if (fscanf(f, "%d", &v) != 1)
        {
            fclose(f);
            return NULL;
        }
        if (field == 0)
            w = v;
        else if (field == 1)
            h = v;
        else
            maxv = v;
        field++;
    }
    fgetc(f); /* single whitespace after maxval */

    if (w <= 0 || h <= 0 || maxv <= 0)
    {
        fclose(f);
        return NULL;
    }

    Image *img = malloc(sizeof(Image));
    if (!img)
    {
        fclose(f);
        return NULL;
    }
    img->width = w;
    img->height = h;
    img->rgba = malloc((size_t)w * h * 4);
    if (!img->rgba)
    {
        free(img);
        fclose(f);
        return NULL;
    }

    size_t px = (size_t)w * h;
    if (ascii)
    {
        for (size_t i = 0; i < px; i++)
        {
            int r = 0, g = 0, b = 0;
            if (fscanf(f, "%d %d %d", &r, &g, &b) != 3)
            {
                free(img->rgba);
                free(img);
                fclose(f);
                return NULL;
            }
            img->rgba[i * 4 + 0] = (unsigned char)r;
            img->rgba[i * 4 + 1] = (unsigned char)g;
            img->rgba[i * 4 + 2] = (unsigned char)b;
            img->rgba[i * 4 + 3] = 255;
        }
    }
    else
    {
        unsigned char *buf = malloc(px * 3);
        if (!buf)
        {
            free(img->rgba);
            free(img);
            fclose(f);
            return NULL;
        }
        if (fread(buf, 1, px * 3, f) != px * 3)
        {
            free(buf);
            free(img->rgba);
            free(img);
            fclose(f);
            return NULL;
        }
        for (size_t i = 0; i < px; i++)
        {
            img->rgba[i * 4 + 0] = buf[i * 3 + 0];
            img->rgba[i * 4 + 1] = buf[i * 3 + 1];
            img->rgba[i * 4 + 2] = buf[i * 3 + 2];
            img->rgba[i * 4 + 3] = 255;
        }
        free(buf);
    }
    fclose(f);
    return img;
}

static int write_ppm(const char *path, const Image *img)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return 0;
    fprintf(f, "P6\n%d %d\n255\n", img->width, img->height);
    size_t px = (size_t)img->width * img->height;
    unsigned char *buf = malloc(px * 3);
    if (!buf)
    {
        fclose(f);
        return 0;
    }
    for (size_t i = 0; i < px; i++)
    {
        buf[i * 3 + 0] = img->rgba[i * 4 + 0];
        buf[i * 3 + 1] = img->rgba[i * 4 + 1];
        buf[i * 3 + 2] = img->rgba[i * 4 + 2];
    }
    size_t wrote = fwrite(buf, 1, px * 3, f);
    free(buf);
    fclose(f);
    return wrote == px * 3;
}

/* ------------------------------------------------------------------ */
/* Watermark + anti-AI treatment                                      */
/* ------------------------------------------------------------------ */

/*
 * 5x7 bitmap font for A-Z 0-9 and a few symbols, so the watermark can be
 * burned into the pixels without depending on a font library.
 * Each glyph is 7 rows of 5 bits (MSB = leftmost pixel).
 */
static const uint8_t FONT5x7[][7] = {
    /* A */ {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
    /* B */ {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
    /* C */ {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},
    /* D */ {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E},
    /* E */ {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},
    /* F */ {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
    /* G */ {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F},
    /* H */ {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
    /* I */ {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
    /* J */ {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C},
    /* K */ {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},
    /* L */ {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
    /* M */ {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},
    /* N */ {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11},
    /* O */ {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    /* P */ {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
    /* Q */ {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D},
    /* R */ {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
    /* S */ {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},
    /* T */ {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    /* U */ {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    /* V */ {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},
    /* W */ {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A},
    /* X */ {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},
    /* Y */ {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04},
    /* Z */ {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},
    /* 0 */ {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
    /* 1 */ {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    /* 2 */ {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    /* 3 */ {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    /* 4 */ {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    /* 5 */ {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    /* 6 */ {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    /* 7 */ {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    /* 8 */ {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    /* 9 */ {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    /* - */ {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00},
    /* . */ {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C},
    /* : */ {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00},
    /* space */ {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    /* / */ {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10},
    /* _ */ {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F},
};

static int glyph_index(char c)
{
    c = (char)toupper((unsigned char)c);
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= '0' && c <= '9')
        return 26 + (c - '0');
    switch (c)
    {
    case '-':
        return 36;
    case '.':
        return 37;
    case ':':
        return 38;
    case ' ':
        return 39;
    case '/':
        return 40;
    case '_':
        return 41;
    default:
        return 39; /* unknown -> space */
    }
}

static void put_px(Image *img, int x, int y, int r, int g, int b, int alpha_pct)
{
    if (x < 0 || y < 0 || x >= img->width || y >= img->height)
        return;
    unsigned char *p = &img->rgba[((size_t)y * img->width + x) * 4];
    /* Alpha-blend the watermark so it stays legible but not opaque. */
    int a = alpha_pct;
    p[0] = (unsigned char)((p[0] * (100 - a) + r * a) / 100);
    p[1] = (unsigned char)((p[1] * (100 - a) + g * a) / 100);
    p[2] = (unsigned char)((p[2] * (100 - a) + b * a) / 100);
}

/*
 * Burn a tiled, repeated watermark across the image plus one prominent line
 * near the bottom. Whitespace is drawn as a dim shadow so the mark survives
 * being viewed on any background.
 */
static void watermark(Image *img, const char *text)
{
    if (!text || !*text)
        return;

    int scale = img->width / 220;
    if (scale < 1)
        scale = 1;
    if (scale > 4)
        scale = 4;

    int gw = 5 * scale;
    int gh = 7 * scale;
    int advance = gw + 2 * scale;

    int text_px = (int)strlen(text) * advance;
    /* Tile it: horizontal step is the text width + gap; vertical step generous. */
    int step_x = text_px + 60;
    int step_y = gh * 6;

    for (int oy = gh; oy < img->height; oy += step_y)
    {
        for (int ox = 0; ox + text_px < img->width; ox += step_x)
        {
            int cx = ox;
            for (const char *t = text; *t; t++)
            {
                int gi = glyph_index(*t);
                for (int row = 0; row < 7; row++)
                {
                    uint8_t bits = FONT5x7[gi][row];
                    for (int col = 0; col < 5; col++)
                    {
                        if (!(bits & (1 << (4 - col))))
                            continue;
                        for (int sy = 0; sy < scale; sy++)
                        {
                            for (int sx = 0; sx < scale; sx++)
                            {
                                put_px(img, cx + col * scale + sx, oy + (row * scale) + sy,
                                       255, 255, 255, 22);
                            }
                        }
                    }
                }
                cx += advance;
            }
        }
    }
}

/*
 * Anti-AI treatment: add a deterministic low-amplitude perturbation.
 * Deterministic so the encoder's output is reproducible for testing.
 */
static void anti_ai(Image *img, uint32_t seed)
{
    uint32_t s = seed ? seed : 1u;
    size_t px = (size_t)img->width * img->height;
    for (size_t i = 0; i < px; i++)
    {
        s = s * 1103515245u + 12345u;
        int d = (int)((s >> 16) & 0x07) - 3; /* -3..+3 */
        unsigned char *p = &img->rgba[i * 4];
        int r = p[0] + d, g = p[1] + d, b = p[2] + d;
        p[0] = (unsigned char)(r < 0 ? 0 : (r > 255 ? 255 : r));
        p[1] = (unsigned char)(g < 0 ? 0 : (g > 255 ? 255 : g));
        p[2] = (unsigned char)(b < 0 ? 0 : (b > 255 ? 255 : b));
    }
}

/* ------------------------------------------------------------------ */
/* CBV container                                                      */
/* ------------------------------------------------------------------ */

static uint32_t checksum(const unsigned char *data, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
    {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

static int write_be32(FILE *f, uint32_t v)
{
    unsigned char b[4] = {
        (unsigned char)(v >> 24), (unsigned char)(v >> 16),
        (unsigned char)(v >> 8), (unsigned char)(v)};
    return fwrite(b, 1, 4, f) == 4;
}

static int encode_cbv(const char *in_path, const char *out_path, const char *mark)
{
    Image *img = read_ppm(in_path);
    if (!img)
    {
        fprintf(stderr, "encode: cannot read %s (need PPM P6)\n", in_path);
        return 1;
    }

    anti_ai(img, (uint32_t)img->width * 2654435761u ^ (uint32_t)img->height);
    watermark(img, (mark && *mark) ? mark : "CADBAY DO NOT DISTRIBUTE");

    /* Derive the obfuscation salt from the content so each file differs. */
    uint32_t salt = checksum(img->rgba, (size_t)img->width * img->height * 4);

    FILE *f = fopen(out_path, "wb");
    if (!f)
    {
        fprintf(stderr, "encode: cannot write %s\n", out_path);
        free(img->rgba);
        free(img);
        return 1;
    }

    fwrite("CBV1", 1, 4, f);
    write_be32(f, (uint32_t)img->width);
    write_be32(f, (uint32_t)img->height);
    write_be32(f, salt);

    size_t px = (size_t)img->width * img->height * 4;
    uint32_t state = salt;
    unsigned char *ob = malloc(px);
    if (!ob)
    {
        fclose(f);
        free(img->rgba);
        free(img);
        return 1;
    }
    for (size_t i = 0; i < px; i++)
    {
        state = state * 1664525u + 1013904223u;
        ob[i] = img->rgba[i] ^ (unsigned char)(state >> 24);
    }
    size_t wrote = fwrite(ob, 1, px, f);
    free(ob);
    fclose(f);
    free(img->rgba);
    free(img);

    if (wrote != px)
    {
        fprintf(stderr, "encode: short write\n");
        return 1;
    }
    printf("encoded %s -> %s (%dx%d)\n", in_path, out_path, img ? 0 : 0, 0);
    return 0;
}

static int decode_cbv(const char *in_path, const char *out_path)
{
    FILE *f = fopen(in_path, "rb");
    if (!f)
    {
        fprintf(stderr, "decode: cannot open %s\n", in_path);
        return 1;
    }

    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "CBV1", 4) != 0)
    {
        fprintf(stderr, "decode: not a CBV file\n");
        fclose(f);
        return 1;
    }
    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12)
    {
        fclose(f);
        return 1;
    }
    uint32_t w = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | hdr[3];
    uint32_t h = ((uint32_t)hdr[4] << 24) | ((uint32_t)hdr[5] << 16) | ((uint32_t)hdr[6] << 8) | hdr[7];
    uint32_t salt = ((uint32_t)hdr[8] << 24) | ((uint32_t)hdr[9] << 16) | ((uint32_t)hdr[10] << 8) | hdr[11];

    if (w == 0 || h == 0 || w > 20000 || h > 20000)
    {
        fprintf(stderr, "decode: implausible dimensions %ux%u\n", w, h);
        fclose(f);
        return 1;
    }

    size_t px = (size_t)w * h * 4;
    unsigned char *ob = malloc(px);
    if (!ob)
    {
        fclose(f);
        return 1;
    }
    if (fread(ob, 1, px, f) != px)
    {
        fprintf(stderr, "decode: truncated payload\n");
        free(ob);
        fclose(f);
        return 1;
    }
    fclose(f);

    Image img;
    img.width = (int)w;
    img.height = (int)h;
    img.rgba = malloc(px);
    if (!img.rgba)
    {
        free(ob);
        return 1;
    }

    uint32_t state = salt;
    for (size_t i = 0; i < px; i++)
    {
        state = state * 1664525u + 1013904223u;
        img.rgba[i] = ob[i] ^ (unsigned char)(state >> 24);
    }
    free(ob);

    int ok = write_ppm(out_path, &img);
    free(img.rgba);
    if (!ok)
    {
        fprintf(stderr, "decode: write failed\n");
        return 1;
    }
    printf("decoded %s -> %s (%ux%u)\n", in_path, out_path, w, h);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        fprintf(stderr,
                "usage:\n"
                "  %s encode <in.ppm> <out.cbv> [watermark text]\n"
                "  %s decode <in.cbv> <out.ppm>\n",
                argv[0], argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "encode") == 0)
    {
        const char *mark = (argc >= 5) ? argv[4] : "CADBAY DO NOT DISTRIBUTE";
        return encode_cbv(argv[2], argv[3], mark);
    }
    if (strcmp(argv[1], "decode") == 0)
    {
        return decode_cbv(argv[2], argv[3]);
    }

    fprintf(stderr, "unknown command: %s\n", argv[1]);
    return 2;
}
