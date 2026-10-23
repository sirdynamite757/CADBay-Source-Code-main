/*
 * seed_test_data.c - fill CADBay with fake RFPs and ads for testing.
 *
 * Writes RFPs in the format the webserver parses (/api/rfps, /api/rfp/<id>):
 *
 *     RFP ID: <id>
 *     Owner: <user>
 *     Title: <project name>
 *     Short Description: <one line>
 *     Long Description: <one line>
 *     Budget: <price>
 *     Tier: <ad|enterprise|premium|free>
 *     Project Type: <project type>
 *     Program: <software>
 *
 * Writes ads under ads/<block|banner|sidebar>/<name>/ as either:
 *   index.html            -> themed card that pulls in ../../css/ad-theme.css
 *   image.png + conf.conf -> image ad with link/description metadata
 *
 * Everything it creates is named with the prefixes "testrfp_" / "testad_"
 * so -c (clean) can delete only the seeded data and leave real data alone.
 *
 * Build:  gcc -Wall -Wextra -O2 -o tools/seed_test_data tools/seed_test_data.c
 * Or:     make seed
 *
 * Usage:
 *   ./tools/seed_test_data                    60 RFPs + 24 ads
 *   ./tools/seed_test_data -r 200 -a 60       200 RFPs + 60 ads
 *   ./tools/seed_test_data -r 0 -a 40         ads only
 *   ./tools/seed_test_data -r 25 -a 0         RFPs only
 *   ./tools/seed_test_data -n                 dry run, write nothing
 *   ./tools/seed_test_data -c                 delete seeded data and exit
 *   ./tools/seed_test_data -s 42              change the random seed
 *
 * If RFPs/<tier> resolves into the ads tree (for example RFPs/ad is a symlink
 * to ads/block), that tier is skipped with a warning so RFP files are never
 * left lying amongst the ads.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>

#define REPO_ROOT_MAX 1024
#define PATH_MAX_LEN 2048
#define LINE_MAX_LEN 4096

#define RFP_PREFIX "testrfp"
#define AD_PREFIX "testad"

static char g_repo_root[REPO_ROOT_MAX];

/* ---------------------------------------------------------------- */
/* Random numbers                                                    */
/* ---------------------------------------------------------------- */

static uint32_t g_rng_state = 1337u;

static uint32_t rng_next(void)
{
    /* xorshift32: good enough for choosing test data */
    uint32_t x = g_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng_state = x;
    return x;
}

static int rng_range(int low, int high)
{
    if (high <= low)
        return low;
    return low + (int)(rng_next() % (uint32_t)(high - low + 1));
}

/* ---------------------------------------------------------------- */
/* Test data tables                                                  */
/* ---------------------------------------------------------------- */

/* Every entry is a project type, the software commonly used for it, some
 * plausible job titles, and a sentence or two of brief text. */

typedef struct
{
    const char *project_type;
    const char *programs[10];
    const char *titles[6];
    const char *blurbs[4];
} ProjectSpec;

static const ProjectSpec g_projects[] = {
    {"engineering",
     {"Solidworks", "AutoCAD", "FreeCAD", "Fusion 360", "Onshape", "CATIA", "Creo", "NX", "Inventor", NULL},
     {"Bracket Assembly", "Gearbox Housing", "Frame Weldment", "Hydraulic Manifold", "Bearing Carrier", NULL},
     {"Deliver a fully constrained parametric model with a manufacturing drawing set.",
      "Reverse engineer an existing part and produce a watertight STEP exchange model.",
      "Design a lightweight structural assembly that passes a 5x static load case.",
      NULL}},
    {"architectural",
     {"Revit", "SketchUp", "ArchiCAD", "Vectorworks", "Chief Architect", "Rhino", NULL},
     {"Cabin Concept", "Retail Fit-out", "Staircase Detail", "Facade Study", "Loft Conversion", NULL},
     {"Produce a 3D model plus plan and elevation sheets suitable for a planning submission.",
      "Model the existing structure and show the proposed extension in two options.",
      NULL}},
    {"industrial",
     {"Solidworks", "AutoCAD", "Fusion 360", "CATIA", "Creo", NULL},
     {"Conveyor Run", "Pallet Rack", "Assembly Jig", "Robot Workcell", "Packaging Line", NULL},
     {"Lay out the full line with clearance envelopes and an interference check report.",
      "Design a jig that holds the part within 0.2mm through the weld cycle.",
      NULL}},
    {"product-visualization",
     {"Blender", "Maya", "3ds Max", "Cinema 4D", "Houdini", "KeyShot", "V-Ray", NULL},
     {"Hero Renders", "Turntable Animation", "Exploded View", "Packshot Set", "AR Asset", NULL},
     {"Five studio renders plus one exploded view for the marketing launch page.",
      "Photoreal turntable at 1080p, 30fps, 10 seconds, with transparent background.",
      NULL}},
    {"webpage-development",
     {"html", "css", "javascript", "react", "angular", "vue", "bootstrap", NULL},
     {"Landing Page", "Quote Calculator", "3D Viewer Widget", "Parts Catalog", "Portfolio Site", NULL},
     {"Responsive marketing page with a contact form and basic analytics hooks.",
      "Build an embeddable widget that loads a glTF model and exposes orbit controls.",
      NULL}},
    {"game-programming",
     {"Unity", "Unreal Engine", "Godot", "GameMaker Studio", "Cocos2d-x", NULL},
     {"Inventory System", "Save Load Layer", "Procedural Level Gen", "Combat Prototype", "Netcode Spike", NULL},
     {"Implement a data-driven inventory with stacking, tooltips and persistence.",
      "Write a deterministic procedural generator for cave layouts with seeded runs.",
      NULL}},
    {"tool-programming",
     {"Unity", "Unreal Engine", "Godot", "Custom Game Engines", NULL},
     {"Level Editor Plugin", "Batch Renamer", "Asset Validator", "Build Pipeline Tool", "Shader Inspector", NULL},
     {"Editor extension that validates naming conventions across the asset database.",
      "CLI tool that compiles and packages builds for Windows, Linux and macOS.",
      NULL}},
    {"game-texturing",
     {"Substance Painter", "Blender", "3ds Max", "Maya", NULL},
     {"Weapon Set", "Environment Kit", "Character PBR Set", "Prop Library", "Trim Sheet", NULL},
     {"PBR textures at 2K for a six-piece environment kit, tileable and optimised.",
      "Three material variants of a weapon skin using the provided UV layout.",
      NULL}},
    {"game-modeling",
     {"Blender", "Maya", "3ds Max", "ZBrush", "Modo", NULL},
     {"Modular Dungeon", "Vehicle Kit", "Creature Base Mesh", "Prop Pack", "Hard Surface Kit", NULL},
     {"Low-poly game-ready models with clean topology and baked normals.",
      "Modular wall set with 12 snap-together pieces, under 40k tris each.",
      NULL}},
    {"game-animation",
     {"Blender", "Maya", "Toon Boom Harmony", "Adobe Animate", "Houdini", NULL},
     {"Locomotion Set", "Attack Combos", "Cinematic Cutscene", "Facial Rig", "Creature Cycles", NULL},
     {"Rig and animate a 6-clip locomotion set compatible with the provided skeleton.",
      "Two-minute in-engine cinematic with camera, lipsync and prop animation.",
      NULL}},
    {"stock-images",
     {"jpg", "webp", "png", "svg", NULL},
     {"Blueprint Textures", "CAD Blueprint Pack", "Engineering Icons", "Grid Backgrounds", "Isometric Icons", NULL},
     {"50 royalty-free blueprint-style backgrounds at 4K, sRGB, JPEG.",
      "40 flat icons in SVG plus PNG exports at 256px and 512px.",
      NULL}},
    {"other",
     {"Other", NULL},
     {"Misc Modelling Job", "CAD Cleanup", "Format Conversion", "Audit and Fix", "Data Prep", NULL},
     {"Scope described in the attached brief; priced as a fixed-fee milestone.",
      "Convert a mixed bag of files into a single consistent exchange format.",
      NULL}},
};

#define PROJECT_COUNT ((int)(sizeof(g_projects) / sizeof(g_projects[0])))

static const char *g_owners[] = {
    "atlas_works", "skynet_labs", "nova_design", "blueorbit", "forge_and_line",
    "hexbolt", "northlight", "cinder_studio", "apex_cad", "meridian3d"};
#define OWNER_COUNT ((int)(sizeof(g_owners) / sizeof(g_owners[0])))

static const char *g_tiers[] = {"ad", "enterprise", "premium", "free"};
static const int g_tier_weights[] = {1, 2, 4, 3};
#define TIER_COUNT ((int)(sizeof(g_tiers) / sizeof(g_tiers[0])))

static const long g_budgets[] = {250, 500, 1200, 2500, 4800, 7500, 12000, 24000};
#define BUDGET_COUNT ((int)(sizeof(g_budgets) / sizeof(g_budgets[0])))

static const char *g_clients[] = {"startup", "studio", "OEM", "supplier", "indie team"};
#define CLIENT_COUNT ((int)(sizeof(g_clients) / sizeof(g_clients[0])))

typedef struct
{
    const char *headline;
    const char *blurb;
    const char *link;
    const char *cta;
} AdTemplate;

static const AdTemplate g_ad_templates[] = {
    {"Precision Tools Pro", "Industrial-grade CAD add-ins for faster drawing sets.", "/premium-upgrade", "Learn more"},
    {"MeshMarket", "Buy and sell game-ready 3D assets.", "/premium-upgrade", "Browse assets"},
    {"PrintForge", "Send your model to a print farm in one click.", "/premium-upgrade", "Start printing"},
    {"BoltBase", "1M+ standard fasteners, ready to drop into your assembly.", "/premium-upgrade", "Get the library"},
    {"RenderRush", "Cloud rendering for product visualisation teams.", "/premium-upgrade", "Try it free"},
    {"SketchSchool", "Learn parametric modelling in 30 days.", "/premium-upgrade", "Start learning"},
    {"Gridline Studio", "Architectural visualisation, delivered in 72h.", "/premium-upgrade", "Request a quote"},
    {"PartPath", "Automated BOM extraction from any CAD file.", "/premium-upgrade", "See it work"},
    {"ToleranceLab", "GD&T checking and tolerance stack analysis.", "/premium-upgrade", "Book a demo"},
    {"VoxelVault", "Texture subscriptions for indie game studios.", "/premium-upgrade", "View packs"},
    {"CamWorks Cloud", "CAM toolpaths generated straight from your assembly.", "/premium-upgrade", "Get started"},
    {"Node Nerd", "Custom Unity and Unreal tooling, on retainer.", "/premium-upgrade", "Hire a dev"},
};
#define AD_TEMPLATE_COUNT ((int)(sizeof(g_ad_templates) / sizeof(g_ad_templates[0])))

static const char *g_ad_types[] = {"block", "banner", "sidebar"};
#define AD_TYPE_COUNT ((int)(sizeof(g_ad_types) / sizeof(g_ad_types[0])))

/* ---------------------------------------------------------------- */
/* Helpers                                                           */
/* ---------------------------------------------------------------- */

static int tier_is_writable(const char *tier);

/* Pick a folder tier at random, weighted by g_tier_weights. */
static int pick_weighted_tier(void)
{
    int total = 0, roll, i;
    for (i = 0; i < TIER_COUNT; i++)
        total += g_tier_weights[i];
    roll = rng_range(1, total);
    for (i = 0; i < TIER_COUNT; i++)
    {
        roll -= g_tier_weights[i];
        if (roll <= 0)
            return i;
    }
    return TIER_COUNT - 1;
}

/* Like pick_weighted_tier(), but never returns a tier whose RFP folder is
 * tied to an ad folder. Returns -1 only if every tier is tied. */
static int pick_writable_tier(void)
{
    int attempt;
    for (attempt = 0; attempt < 64; attempt++)
    {
        int index = pick_weighted_tier();
        if (tier_is_writable(g_tiers[index]))
            return index;
    }
    for (attempt = 0; attempt < TIER_COUNT; attempt++)
        if (tier_is_writable(g_tiers[attempt]))
            return attempt;
    return -1;
}

static int count_strings(const char *const *list)
{
    int count = 0;
    while (list[count])
        count++;
    return count;
}

/*
 * RFPs/<tier> is written by this tool while ads/<type> folders are written
 * into the ad tree. In some checkouts the RFP tier folder is itself a symlink
 * into the ad tree (for example RFPs/ad -> ads/block), so writing there would
 * drop RFP files among the ads. Report every ad folder a tier resolves into so
 * the caller can refuse to use it.
 */
static int path_is_inside(const char *candidate, const char *root)
{
    size_t length = strlen(root);
    return strncmp(candidate, root, length) == 0 &&
           (candidate[length] == '\0' || candidate[length] == '/');
}

/* Returns 1 unless RFPs/<tier> actually lives inside the ads tree. */
static int tier_is_writable(const char *tier)
{
    char rfp_dir[PATH_MAX_LEN];
    char ads_root[PATH_MAX_LEN];
    char *resolved_rfp;
    char *resolved_ads;
    int writable;

    snprintf(rfp_dir, sizeof(rfp_dir), "%s/RFPs/%s", g_repo_root, tier);
    snprintf(ads_root, sizeof(ads_root), "%s/ads", g_repo_root);

    resolved_ads = realpath(ads_root, NULL);
    resolved_rfp = realpath(rfp_dir, NULL);
    if (!resolved_ads || !resolved_rfp)
    {
        free(resolved_ads);
        free(resolved_rfp);
        return 1; /* nothing exists yet, so the tier is safe to create */
    }
    writable = !path_is_inside(resolved_rfp, resolved_ads);
    free(resolved_ads);
    free(resolved_rfp);
    return writable;
}

/* mkdir -p */
static int make_directories(const char *path)
{
    char buffer[PATH_MAX_LEN];
    size_t length = strlen(path);
    size_t i;

    if (length >= sizeof(buffer))
        return 0;
    memcpy(buffer, path, length + 1);

    for (i = 1; i <= length; i++)
    {
        if (buffer[i] == '/' || buffer[i] == '\0')
        {
            /* skip the empty segment produced by a trailing slash */
            if (buffer[i] == '/' && i + 1 == length)
                continue;
            {
                char saved = buffer[i];
                buffer[i] = '\0';
                if (mkdir(buffer, 0775) != 0 && errno != EEXIST)
                    return 0;
                buffer[i] = saved;
            }
        }
    }
    return 1;
}

static int write_file(const char *path, const char *contents)
{
    FILE *file = fopen(path, "w");
    if (!file)
        return 0;
    if (fputs(contents, file) == EOF)
    {
        fclose(file);
        return 0;
    }
    return fclose(file) == 0;
}

/* ---------------------------------------------------------------- */
/* Minimal PNG writer (so image ads need no extra libraries)         */
/* ---------------------------------------------------------------- */

static void put_u32_be(unsigned char *out, uint32_t value)
{
    out[0] = (unsigned char)(value >> 24);
    out[1] = (unsigned char)(value >> 16);
    out[2] = (unsigned char)(value >> 8);
    out[3] = (unsigned char)value;
}

static int write_png_chunk(FILE *file, const char *kind, const unsigned char *data, size_t length)
{
    unsigned char header[4];
    unsigned char crc_bytes[4];
    uint32_t crc = 0;
    size_t i;

    put_u32_be(header, (uint32_t)length);
    if (fwrite(header, 1, 4, file) != 4)
        return 0;
    if (fwrite(kind, 1, 4, file) != 4)
        return 0;
    if (length && fwrite(data, 1, length, file) != length)
        return 0;

    /* CRC over chunk type + data (standard PNG CRC-32, zlib polynomial) */
    {
        static uint32_t table[256];
        static int table_ready = 0;
        if (!table_ready)
        {
            for (i = 0; i < 256; i++)
            {
                uint32_t c = (uint32_t)i;
                int k;
                for (k = 0; k < 8; k++)
                    c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                table[i] = c;
            }
            table_ready = 1;
        }
        crc = 0xFFFFFFFFu;
        for (i = 0; i < 4; i++)
            crc = table[(crc ^ (unsigned char)kind[i]) & 0xFF] ^ (crc >> 8);
        for (i = 0; i < length; i++)
            crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
        crc ^= 0xFFFFFFFFu;
    }
    put_u32_be(crc_bytes, crc);
    return fwrite(crc_bytes, 1, 4, file) == 4;
}

static int write_ad_png(const char *path, int width, int height, int seed)
{
    unsigned char ihdr[13];
    unsigned char *raw;
    size_t raw_length;
    int x, y, ok;
    FILE *file;
    int top[3], bottom[3];
    uint32_t saved_state = g_rng_state;

    g_rng_state = (uint32_t)seed * 2654435761u + 1u;
    top[0] = rng_range(10, 60);
    top[1] = rng_range(10, 60);
    top[2] = rng_range(30, 90);
    bottom[0] = rng_range(100, 220);
    bottom[1] = rng_range(30, 90);
    bottom[2] = rng_range(40, 120);
    g_rng_state = saved_state;

    raw_length = (size_t)height * (1 + (size_t)width * 3);
    raw = (unsigned char *)malloc(raw_length);
    if (!raw)
        return 0;

    for (y = 0; y < height; y++)
    {
        size_t row = (size_t)y * (1 + (size_t)width * 3);
        raw[row++] = 0; /* filter type: none */
        for (x = 0; x < width; x++)
        {
            double tx = width > 1 ? (double)x / (double)(width - 1) : 0.0;
            double ty = height > 1 ? (double)y / (double)(height - 1) : 0.0;
            double t = (tx + ty) / 2.0;
            raw[row++] = (unsigned char)(top[0] + (bottom[0] - top[0]) * t);
            raw[row++] = (unsigned char)(top[1] + (bottom[1] - top[1]) * t);
            raw[row++] = (unsigned char)(top[2] + (bottom[2] - top[2]) * t);
        }
    }

    file = fopen(path, "wb");
    if (!file)
    {
        free(raw);
        return 0;
    }

    ok = fwrite("\x89PNG\r\n\x1a\n", 1, 8, file) == 8;
    put_u32_be(ihdr, (uint32_t)width);
    put_u32_be(ihdr + 4, (uint32_t)height);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 2;  /* colour type: truecolour RGB */
    ihdr[10] = 0; /* compression */
    ihdr[11] = 0; /* filter */
    ihdr[12] = 0; /* interlace */
    ok = ok && write_png_chunk(file, "IHDR", ihdr, sizeof(ihdr));

    /* IDAT needs the zlib stream wrapped in its own length/CRC, so build it
       in memory first (stored blocks are a few bytes larger than raw). */
    if (ok)
    {
        size_t blocks = (raw_length + 65534) / 65535;
        size_t idat_length = 2 + blocks * 5 + raw_length + 4;
        unsigned char *idat = (unsigned char *)malloc(idat_length);
        if (!idat)
        {
            ok = 0;
        }
        else
        {
            size_t offset = 0, consumed = 0;
            idat[offset++] = 0x78;
            idat[offset++] = 0x01;
            while (consumed < raw_length)
            {
                size_t chunk = raw_length - consumed;
                int final = 0;
                if (chunk > 65535)
                    chunk = 65535;
                if (consumed + chunk >= raw_length)
                    final = 1;
                idat[offset++] = (unsigned char) final;
                idat[offset++] = (unsigned char)(chunk & 0xFF);
                idat[offset++] = (unsigned char)((chunk >> 8) & 0xFF);
                idat[offset++] = (unsigned char)(~chunk & 0xFF);
                idat[offset++] = (unsigned char)((~chunk >> 8) & 0xFF);
                memcpy(idat + offset, raw + consumed, chunk);
                offset += chunk;
                consumed += chunk;
            }
            {
                uint32_t a = 1, b = 0;
                size_t i;
                for (i = 0; i < raw_length; i++)
                {
                    a = (a + raw[i]) % 65521u;
                    b = (b + a) % 65521u;
                }
                put_u32_be(idat + offset, (b << 16) | a);
                offset += 4;
            }
            ok = write_png_chunk(file, "IDAT", idat, offset);
            free(idat);
        }
    }
    ok = ok && write_png_chunk(file, "IEND", NULL, 0);
    if (fclose(file) != 0)
        ok = 0;
    free(raw);
    return ok;
}

/* ---------------------------------------------------------------- */
/* RFP generation                                                    */
/* ---------------------------------------------------------------- */

static int write_rfp(int index, const char *tier, const ProjectSpec *project,
                     const char *program, const char *title, const char *owner,
                     const char *short_description, const char *long_description,
                     long budget)
{
    char path[PATH_MAX_LEN + 96];
    char rfp_id[64];
    char tier_dir[PATH_MAX_LEN];
    char body[LINE_MAX_LEN];
    int written;

    snprintf(rfp_id, sizeof(rfp_id), RFP_PREFIX "_%04d", index);
    snprintf(tier_dir, sizeof(tier_dir), "%s/RFPs/%s", g_repo_root, tier);
    if (!make_directories(tier_dir))
        return 0;
    snprintf(path, sizeof(path), "%s/%s.txt", tier_dir, rfp_id);

    written = snprintf(body, sizeof(body),
                       "RFP ID: %s\n"
                       "Owner: %s\n"
                       "Title: %s\n"
                       "Short Description: %s\n"
                       "Long Description: %s\n"
                       "Budget: %ld.00\n"
                       "Tier: %s\n"
                       "Project Type: %s\n"
                       "Program: %s\n",
                       rfp_id, owner, title, short_description, long_description,
                       budget, tier, project->project_type, program);

    if (written < 0 || (size_t)written >= sizeof(body))
        return 0;
    return write_file(path, body);
}

/* ---------------------------------------------------------------- */
/* Ad generation                                                     */
/* ---------------------------------------------------------------- */

static int ad_size_for(const char *ad_type, int *width, int *height)
{
    if (strcmp(ad_type, "block") == 0)
    {
        *width = 300;
        *height = 250;
        return 1;
    }
    if (strcmp(ad_type, "banner") == 0)
    {
        *width = 728;
        *height = 90;
        return 1;
    }
    if (strcmp(ad_type, "sidebar") == 0)
    {
        *width = 160;
        *height = 600;
        return 1;
    }
    return 0;
}

static int write_ad(int index, const char *ad_type, const AdTemplate *tpl, int use_html)
{
    char folder[PATH_MAX_LEN];
    char name[128];

    snprintf(name, sizeof(name), AD_PREFIX "_%s_%02d", ad_type, index);
    snprintf(folder, sizeof(folder), "%s/ads/%s/%s", g_repo_root, ad_type, name);
    if (!make_directories(folder))
        return 0;

    if (use_html)
    {
        char path[PATH_MAX_LEN + 32];
        char html[LINE_MAX_LEN * 2];
        snprintf(path, sizeof(path), "%s/index.html", folder);
        snprintf(html, sizeof(html),
                 "<link rel=\"stylesheet\" href=\"../../css/ad-theme.css\">\n"
                 "<div class=\"ad-card\">\n"
                 "    <span class=\"ad-label\">Sponsored</span>\n"
                 "    <h3>%s</h3>\n"
                 "    <p>%s</p>\n"
                 "    <a href=\"%s\" class=\"ad-button\">%s</a>\n"
                 "</div>\n"
                 "<script src=\"../../js/ad-theme.js\"></script>\n",
                 tpl->headline, tpl->blurb, tpl->link, tpl->cta);
        return write_file(path, html);
    }
    else
    {
        char png_path[PATH_MAX_LEN + 32];
        char conf_path[PATH_MAX_LEN + 32];
        char conf[LINE_MAX_LEN * 2];
        int width, height;

        if (!ad_size_for(ad_type, &width, &height))
            return 0;
        snprintf(png_path, sizeof(png_path), "%s/image.png", folder);
        snprintf(conf_path, sizeof(conf_path), "%s/conf.conf", folder);
        snprintf(conf, sizeof(conf), "link: %s\ndescription: %s - %s\n",
                 tpl->link, tpl->headline, tpl->blurb);

        return write_ad_png(png_path, width, height, index) && write_file(conf_path, conf);
    }
}

/* ---------------------------------------------------------------- */
/* Clean                                                             */
/* ---------------------------------------------------------------- */

static int remove_tree(const char *path)
{
    DIR *dir = opendir(path);
    struct dirent *entry;
    int removed = 0;

    if (!dir)
        return 0;
    while ((entry = readdir(dir)) != NULL)
    {
        char child[PATH_MAX_LEN];
        struct stat info;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (stat(child, &info) == 0 && S_ISDIR(info.st_mode))
        {
            removed += remove_tree(child);
        }
        else
        {
            if (remove(child) == 0)
                removed++;
        }
    }
    closedir(dir);
    if (rmdir(path) == 0)
        removed++;
    return removed;
}

static int clean_seeded_data(void)
{
    int removed = 0;
    int i;

    for (i = 0; i < TIER_COUNT; i++)
    {
        char folder[PATH_MAX_LEN];
        DIR *dir;
        struct dirent *entry;
        snprintf(folder, sizeof(folder), "%s/RFPs/%s", g_repo_root, g_tiers[i]);
        dir = opendir(folder);
        if (!dir)
            continue;
        while ((entry = readdir(dir)) != NULL)
        {
            char path[PATH_MAX_LEN + 300];
            struct stat info;
            if (strncmp(entry->d_name, RFP_PREFIX, strlen(RFP_PREFIX)) != 0 &&
                strncmp(entry->d_name, AD_PREFIX, strlen(AD_PREFIX)) != 0)
                continue;
            snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name);
            if (stat(path, &info) == 0 && S_ISREG(info.st_mode) && remove(path) == 0)
                removed++;
        }
        closedir(dir);
    }

    for (i = 0; i < AD_TYPE_COUNT; i++)
    {
        char folder[PATH_MAX_LEN];
        DIR *dir;
        struct dirent *entry;
        snprintf(folder, sizeof(folder), "%s/ads/%s", g_repo_root, g_ad_types[i]);
        dir = opendir(folder);
        if (!dir)
            continue;
        while ((entry = readdir(dir)) != NULL)
        {
            char path[PATH_MAX_LEN + 300];
            struct stat info;
            if (strncmp(entry->d_name, AD_PREFIX, strlen(AD_PREFIX)) != 0 &&
                strncmp(entry->d_name, RFP_PREFIX, strlen(RFP_PREFIX)) != 0)
                continue;
            snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name);
            if (stat(path, &info) != 0)
                continue;
            /* RFP files that landed in a tied ad folder are regular files,
             * ad folders are directories. */
            if (S_ISDIR(info.st_mode))
                removed += remove_tree(path);
            else if (remove(path) == 0)
                removed++;
        }
        closedir(dir);
    }

    return removed;
}

/* ---------------------------------------------------------------- */
/* Entry point                                                       */
/* ---------------------------------------------------------------- */

static void resolve_repo_root(const char *argv0)
{
    char *resolved;
    char *slash;

    /* realpath() needs a PATH_MAX-sized buffer, so let it allocate one. */
    resolved = realpath(argv0, NULL);
    if (resolved != NULL)
    {
        slash = strrchr(resolved, '/');
        if (slash)
        {
            *slash = '\0';
            slash = strrchr(resolved, '/'); /* strip the tools/ directory */
            if (slash && strcmp(slash + 1, "tools") == 0)
                *slash = '\0';
            snprintf(g_repo_root, sizeof(g_repo_root), "%s", resolved);
            free(resolved);
            return;
        }
        free(resolved);
    }
    snprintf(g_repo_root, sizeof(g_repo_root), ".");
}

static void usage(const char *program)
{
    printf("Usage: %s [options]\n"
           "  -r N      number of RFPs to create (default 60)\n"
           "  -a N      number of ads to create (default 24)\n"
           "  -s N      random seed (default 1337)\n"
           "  -n        dry run, print what would be written\n"
           "  -c        delete previously seeded test data and exit\n"
           "  -h        show this help\n",
           program);
}

int main(int argc, char **argv)
{
    int rfp_count = 60;
    int ad_count = 24;
    int dry_run = 0;
    int clean = 0;
    int seed = 1337;
    int i, written = 0;
    int arg;

    for (arg = 1; arg < argc; arg++)
    {
        if (strcmp(argv[arg], "-r") == 0 && arg + 1 < argc)
        {
            rfp_count = atoi(argv[++arg]);
        }
        else if (strcmp(argv[arg], "-a") == 0 && arg + 1 < argc)
        {
            ad_count = atoi(argv[++arg]);
        }
        else if (strcmp(argv[arg], "-s") == 0 && arg + 1 < argc)
        {
            seed = atoi(argv[++arg]);
        }
        else if (strcmp(argv[arg], "-n") == 0)
        {
            dry_run = 1;
        }
        else if (strcmp(argv[arg], "-c") == 0)
        {
            clean = 1;
        }
        else if (strcmp(argv[arg], "-h") == 0 || strcmp(argv[arg], "--help") == 0)
        {
            usage(argv[0]);
            return EXIT_SUCCESS;
        }
        else
        {
            fprintf(stderr, "Unknown option: %s\n", argv[arg]);
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    resolve_repo_root(argv[0]);

    if (clean)
    {
        int removed = clean_seeded_data();
        printf("Removed %d seeded test items.\n", removed);
        return EXIT_SUCCESS;
    }

    if (rfp_count < 0 || ad_count < 0)
    {
        fprintf(stderr, "Counts must be zero or more.\n");
        return EXIT_FAILURE;
    }

    g_rng_state = (uint32_t)seed ? (uint32_t)seed : 1u;

    if (rfp_count > 0)
    {
        int writable = 0;
        for (i = 0; i < TIER_COUNT; i++)
            if (tier_is_writable(g_tiers[i]))
                writable++;
        if (writable == 0)
        {
            fprintf(stderr, "No writable RFP tier folders found under %s/RFPs.\n", g_repo_root);
            return EXIT_FAILURE;
        }
        if (writable < TIER_COUNT)
        {
            for (i = 0; i < TIER_COUNT; i++)
            {
                if (!tier_is_writable(g_tiers[i]))
                    fprintf(stderr,
                            "Warning: RFPs/%s resolves into the ads tree, so that tier is skipped.\n",
                            g_tiers[i]);
            }
        }
    }

    for (i = 0; i < rfp_count; i++)
    {
        const ProjectSpec *project = &g_projects[rng_range(0, PROJECT_COUNT - 1)];
        const char *program = project->programs[rng_range(0, count_strings(project->programs) - 1)];
        const char *noun = project->titles[rng_range(0, count_strings(project->titles) - 1)];
        const char *blurb = project->blurbs[rng_range(0, count_strings(project->blurbs) - 1)];
        int tier_index = pick_writable_tier();
        const char *tier;
        const char *owner = g_owners[rng_range(0, OWNER_COUNT - 1)];
        const char *client = g_clients[rng_range(0, CLIENT_COUNT - 1)];
        long budget = g_budgets[rng_range(0, BUDGET_COUNT - 1)];
        char title[256];
        char short_description[512];
        char long_description[1024];

        if (tier_index < 0)
        {
            fprintf(stderr, "Failed to choose a writable RFP tier.\n");
            return EXIT_FAILURE;
        }
        tier = g_tiers[tier_index];

        snprintf(title, sizeof(title), "%s #%d", noun, rng_range(100, 999));
        snprintf(short_description, sizeof(short_description),
                 "%s %s for a %s.", program, noun, client);
        snprintf(long_description, sizeof(long_description),
                 "%s Estimated effort: %d hours.", blurb, rng_range(8, 240));

        if (dry_run)
        {
            printf("  RFPs/%s/" RFP_PREFIX "_%04d.txt\n", tier, i);
        }
        else if (write_rfp(i, tier, project, program, title, owner,
                           short_description, long_description, budget))
        {
            written++;
        }
        else
        {
            fprintf(stderr, "Failed to write RFP %d\n", i);
        }
    }

    for (i = 0; i < ad_count; i++)
    {
        const char *ad_type = g_ad_types[i % AD_TYPE_COUNT];
        const AdTemplate *tpl = &g_ad_templates[rng_range(0, AD_TEMPLATE_COUNT - 1)];
        int use_html = rng_range(0, 1);

        if (dry_run)
        {
            printf("  ads/%s/" AD_PREFIX "_%s_%02d (%s)\n", ad_type, ad_type, i,
                   use_html ? "html" : "image");
        }
        else if (write_ad(i, ad_type, tpl, use_html))
        {
            written++;
        }
        else
        {
            fprintf(stderr, "Failed to write ad %d\n", i);
        }
    }

    if (dry_run)
    {
        printf("Dry run: would write %d RFPs and %d ads.\n", rfp_count, ad_count);
        return EXIT_SUCCESS;
    }

    printf("Seeded %d RFPs and %d ads (%d items total).\n", rfp_count, ad_count, written);
    printf("Remove them later with: %s -c\n", argv[0]);
    return written == rfp_count + ad_count ? EXIT_SUCCESS : EXIT_FAILURE;
}
