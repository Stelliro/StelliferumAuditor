
// ============================================================================
// WEB LOOKUP — Internet-based mod attachment discovery
// ============================================================================
// Uses WinINet (linked in CMakeLists.txt) to query Steam Workshop API and
// DayZ Fandom Wiki.  Extracts classnames from mod descriptions, classifies
// them, infers attachment relationships, and caches results locally.
//
// Steam Workshop API endpoint (no API key required):
//   POST https://api.steampowered.com/ISteamRemoteStorage/GetPublishedFileDetails/v1/
//   Body: itemcount=1&publishedfileids[0]=<WORKSHOP_ID>
//   Returns JSON with publishedfiledetails[].description (BBCode text)
//
// DayZ Fandom Wiki API:
//   GET https://dayz.fandom.com/api.php?action=parse&page=<NAME>&format=json
//   Returns JSON with parse.wikitext["*"] (MediaWiki markup)

#include "web_lookup.h"
#include "auditor.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <wininet.h>
// MSVC uses strtok_s instead of POSIX strtok_r
#define strtok_r strtok_s
#endif

// ============================================================================
// INTERNAL: HTTP REQUEST LAYER (WinINet)
// ============================================================================

// HTTP GET via WinINet.  Returns bytes read, 0 on failure.
static int http_get(void *hInternet, const char *url,
                    char *response, int max_len) {
    if (!hInternet || !url || !response || max_len <= 0) return 0;

#ifdef _WIN32
    HINTERNET hUrl = InternetOpenUrlA(
        (HINTERNET)hInternet, url, NULL, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE |
        INTERNET_FLAG_NO_CACHE_WRITE,
        0);
    if (!hUrl) {
        util_log(SEVERITY_WARNING,
                 "WebLookup: GET failed — InternetOpenUrl error %lu for %s",
                 GetLastError(), url);
        return 0;
    }

    int total = 0;
    DWORD bytes_read = 0;
    char buf[4096];
    while (total < max_len - 1) {
        if (!InternetReadFile(hUrl, buf, sizeof(buf) - 1, &bytes_read) ||
            bytes_read == 0)
            break;
        int copy = ((int)bytes_read < max_len - 1 - total)
                       ? (int)bytes_read
                       : (max_len - 1 - total);
        memcpy(response + total, buf, copy);
        total += copy;
    }
    response[total] = '\0';
    InternetCloseHandle(hUrl);
    return total;
#else
    (void)hInternet; (void)url; (void)response; (void)max_len;
    return 0;
#endif
}

// HTTP POST via WinINet.  Returns bytes read, 0 on failure.
static int http_post(void *hInternet, const char *host, const char *path,
                     const char *post_data,
                     char *response, int max_len) {
    if (!hInternet || !host || !path || !post_data ||
        !response || max_len <= 0)
        return 0;

#ifdef _WIN32
    HINTERNET hConnect = InternetConnectA(
        (HINTERNET)hInternet, host,
        INTERNET_DEFAULT_HTTPS_PORT,
        NULL, NULL,
        INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConnect) {
        util_log(SEVERITY_WARNING,
                 "WebLookup: POST connect failed — error %lu for %s",
                 GetLastError(), host);
        return 0;
    }

    const char *accept_types[] = { "application/json", NULL };
    HINTERNET hRequest = HttpOpenRequestA(
        hConnect, "POST", path,
        NULL, NULL, accept_types,
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD |
        INTERNET_FLAG_NO_CACHE_WRITE,
        0);
    if (!hRequest) {
        util_log(SEVERITY_WARNING,
                 "WebLookup: POST open failed — error %lu for %s%s",
                 GetLastError(), host, path);
        InternetCloseHandle(hConnect);
        return 0;
    }

    // Per-request timeouts: 15 s connect, 30 s receive
    DWORD timeout = 15000;
    InternetSetOptionA(hRequest, INTERNET_OPTION_CONNECT_TIMEOUT,
                       &timeout, sizeof(timeout));
    timeout = 30000;
    InternetSetOptionA(hRequest, INTERNET_OPTION_RECEIVE_TIMEOUT,
                       &timeout, sizeof(timeout));

    const char *headers =
        "Content-Type: application/x-www-form-urlencoded\r\n";
    if (!HttpSendRequestA(hRequest,
                          headers, (DWORD)strlen(headers),
                          (void *)post_data, (DWORD)strlen(post_data))) {
        util_log(SEVERITY_WARNING,
                 "WebLookup: POST send failed — error %lu for %s%s",
                 GetLastError(), host, path);
        InternetCloseHandle(hRequest);
        InternetCloseHandle(hConnect);
        return 0;
    }

    int total = 0;
    DWORD bytes_read = 0;
    char buf[4096];
    while (total < max_len - 1) {
        if (!InternetReadFile(hRequest, buf, sizeof(buf) - 1, &bytes_read) ||
            bytes_read == 0)
            break;
        int copy = ((int)bytes_read < max_len - 1 - total)
                       ? (int)bytes_read
                       : (max_len - 1 - total);
        memcpy(response + total, buf, copy);
        total += copy;
    }
    response[total] = '\0';

    InternetCloseHandle(hRequest);
    InternetCloseHandle(hConnect);
    return total;
#else
    (void)hInternet; (void)host; (void)path; (void)post_data;
    (void)response; (void)max_len;
    return 0;
#endif
}

// ============================================================================
// INTERNAL: TEXT PARSING
// ============================================================================

// Strip Steam BBCode tags ([h1], [b], [list], [*], [url=...], etc.)
// Replaces [*] list items with newlines for easier line-by-line scanning.
static void strip_bbcode(const char *src, char *dst, int max_len) {
    int j = 0;
    for (int i = 0; src[i] && j < max_len - 1; i++) {
        if (src[i] == '[') {
            bool is_list_item = (src[i + 1] == '*' && src[i + 2] == ']');
            while (src[i] && src[i] != ']') i++;
            if (!src[i]) break;
            if (is_list_item && j < max_len - 1)
                dst[j++] = '\n';
            continue;
        }
        dst[j++] = src[i];
    }
    dst[j] = '\0';
}

// Returns true if token looks like a DayZ classname (PascalCase or underscore
// separated, 4-80 chars, starts with letter, not a common English word).
static bool looks_like_classname(const char *token) {
    int len = (int)strlen(token);
    if (len < 4 || len > 80) return false;
    if (!isalpha((unsigned char)token[0])) return false;

    bool has_upper = false;
    bool has_underscore = false;
    for (int i = 0; i < len; i++) {
        char c = token[i];
        if (!isalnum((unsigned char)c) && c != '_') return false;
        if (isupper((unsigned char)c)) has_upper = true;
        if (c == '_') has_underscore = true;
    }
    // Must have either PascalCase or underscores
    if (!has_upper && !has_underscore) return false;

    // Filter noise: common English words and BBCode/HTML fragments
    static const char *noise[] = {
        "Steam", "Workshop", "DayZ", "Server", "Client", "This", "That",
        "With", "From", "Have", "Will", "Changelog", "Version", "Update",
        "Added", "Fixed", "Removed", "Changed", "Note", "Notes", "Thanks",
        "Please", "Items", "Weapons", "Attachments", "Compatible", "Download",
        "Install", "Required", "Mods", "File", "Config", "Type", "Name",
        "Class", "Known", "Description", "Features", "Includes", "Special",
        "About", "Misc", "General", "Other", "Build", "Release", "Patch",
        "HTTPS", "HTTP", "HTML", "NULL", "TRUE", "FALSE", "NONE",
        NULL
    };
    for (int n = 0; noise[n]; n++) {
        if (util_strcasecmp(token, noise[n]) == 0)
            return false;
    }
    return true;
}

// Extract classname-like tokens from plain text.
// Returns number of unique classnames found.
static int extract_classnames(const char *text, char out[][128], int max_items) {
    int count = 0;
    char token[128];
    int tlen = 0;

    for (int i = 0; text[i] && count < max_items; i++) {
        char c = text[i];
        if (isalnum((unsigned char)c) || c == '_') {
            if (tlen < 127) token[tlen++] = c;
        } else {
            if (tlen > 0) {
                token[tlen] = '\0';
                if (looks_like_classname(token)) {
                    // Dedup
                    bool dup = false;
                    for (int d = 0; d < count && !dup; d++) {
                        if (util_strcasecmp(out[d], token) == 0) dup = true;
                    }
                    if (!dup) {
                        strncpy(out[count], token, 127);
                        out[count][127] = '\0';
                        count++;
                    }
                }
                tlen = 0;
            }
        }
    }
    // Handle trailing token
    if (tlen > 0 && count < max_items) {
        token[tlen] = '\0';
        if (looks_like_classname(token)) {
            bool dup = false;
            for (int d = 0; d < count && !dup; d++) {
                if (util_strcasecmp(out[d], token) == 0) dup = true;
            }
            if (!dup) {
                strncpy(out[count], token, 127);
                out[count][127] = '\0';
                count++;
            }
        }
    }
    return count;
}

// Classify a web-extracted classname into a category.
static const char *classify_web_classname(const char *cn) {
    // Weapon attachments
    if (util_str_contains_ci(cn, "Bttstck") ||
        util_str_contains_ci(cn, "Hndgrd") ||
        util_str_contains_ci(cn, "Suppressor") ||
        util_str_contains_ci(cn, "Silencer") ||
        util_str_contains_ci(cn, "Compensator"))
        return "attachment";
    if (util_str_contains_ci(cn, "Mag_") ||
        (util_str_contains_ci(cn, "_Mag") &&
         !util_str_contains_ci(cn, "_Magn")))
        return "attachment";
    if (util_str_contains_ci(cn, "Optic") ||
        util_str_contains_ci(cn, "Scope") ||
        util_str_contains_ci(cn, "ACOG") ||
        util_str_contains_ci(cn, "RDS") ||
        util_str_contains_ci(cn, "Reflex"))
        return "attachment";
    if ((util_str_contains_ci(cn, "Light") || util_str_contains_ci(cn, "TLR")) &&
        (util_str_contains_ci(cn, "Weapon") ||
         util_str_contains_ci(cn, "Flashlight") ||
         util_str_contains_ci(cn, "Pistol") ||
         util_str_contains_ci(cn, "Universal")))
        return "attachment";

    // Equipment attachments (pouches, plates, NVGs, visors)
    if (util_str_contains_ci(cn, "Pouch") ||
        util_str_contains_ci(cn, "Holster"))
        return "equip_attach";
    if (util_str_contains_ci(cn, "ArmorPlate") ||
        (util_str_contains_ci(cn, "Plate") &&
         util_str_contains_ci(cn, "Carrier")))
        return "equip_attach";
    if (util_str_contains_ci(cn, "NVG") ||
        util_str_contains_ci(cn, "Visor") ||
        util_str_contains_ci(cn, "FaceShield"))
        return "equip_attach";

    // Weapons
    if (util_str_contains_ci(cn, "Rifle") ||
        util_str_contains_ci(cn, "_Gun") ||
        util_str_contains_ci(cn, "Pistol") ||
        util_str_contains_ci(cn, "Shotgun") ||
        util_str_contains_ci(cn, "SMG") ||
        util_str_contains_ci(cn, "Launcher"))
        return "weapon";

    // Equipment / clothing
    if (util_str_contains_ci(cn, "Jacket") ||
        util_str_contains_ci(cn, "Pants") ||
        util_str_contains_ci(cn, "Shirt") ||
        util_str_contains_ci(cn, "Vest") ||
        util_str_contains_ci(cn, "Helmet") ||
        util_str_contains_ci(cn, "Pack") ||
        util_str_contains_ci(cn, "Bag") ||
        util_str_contains_ci(cn, "Carrier"))
        return "equipment";

    // Ammo
    if (util_str_contains_ci(cn, "Ammo_") ||
        util_str_contains_ci(cn, "_Ammo"))
        return "ammo";

    return "unknown";
}

// Simple targeted JSON string extraction.
// Finds "key":"value" (with optional whitespace around ':') and extracts value.
// Handles JSON escape sequences: \", \\, \n, \t, \/.
// Returns length of extracted string, 0 on failure.
static int json_extract_string(const char *json, const char *key,
                               char *value, int max_len) {
    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char *pos = strstr(json, search);
    if (!pos) return 0;

    pos += strlen(search);
    // Skip whitespace and colon
    while (*pos && (*pos == ' ' || *pos == ':' || *pos == '\t')) pos++;
    if (*pos != '"') return 0;
    pos++; // skip opening quote

    int i = 0;
    while (*pos && i < max_len - 1) {
        if (*pos == '"') break; // unescaped quote = end of string
        if (*pos == '\\' && *(pos + 1)) {
            pos++;
            switch (*pos) {
                case 'n':  value[i++] = '\n'; break;
                case 't':  value[i++] = '\t'; break;
                case '"':  value[i++] = '"';  break;
                case '\\': value[i++] = '\\'; break;
                case '/':  value[i++] = '/';  break;
                case 'r':  value[i++] = '\r'; break;
                default:   value[i++] = *pos; break;
            }
        } else {
            value[i++] = *pos;
        }
        pos++;
    }
    value[i] = '\0';
    return i;
}

// Extract a Steam ID (unsigned long long) from JSON.
// Searches for "key":"<digits>" and returns the numeric value.
static unsigned long long json_extract_ull(const char *json, const char *key) {
    char buf[64] = {0};
    if (json_extract_string(json, key, buf, sizeof(buf)) <= 0)
        return 0;
    return strtoull(buf, NULL, 10);
}

// ============================================================================
// INTERNAL: ITEM STAT EXTRACTION — Multi-language
// ============================================================================
// Parses Workshop descriptions for item stat patterns like:
//   "MK-V Vest (2.5x Vanilla Plate Carrier)"  → protection_mult=2.5
//   "MMPS Backpack (150 Slots)"                → slot_count=150
//   "Assault Pack (80 Slots + Weapon)"          → slot_count=80, has_weapon_slot=true
//   "Armored Helmet (1.5x Vanilla ...)"         → protection_mult=1.5
//
// Supports multiple languages (EN/DE/RU/FR) since many DayZ mod authors
// write descriptions in their native language:
//   DE: "150 Plätze", "2.5x Standard Plattenträger", "Waffe"
//   RU: "150 Слотов", "Оружие"
//   FR: "150 Emplacements", "Arme"

// Multi-language keyword tables for stat pattern matching.
// Each NULL-terminated array covers the same concept across languages.

// Words meaning "slot" / "inventory space"
static const char *STAT_SLOT_WORDS[] = {
    "Slot", "Platz", "Plätze", "Inventar",           // EN, DE
    "Слот",  "Ячеек", "Ячейк",                        // RU
    "Emplacement", "Place",                            // FR
    NULL
};

// Words identifying a reference/baseline item (used with Nx multiplier)
static const char *STAT_REFERENCE_WORDS[] = {
    "Vanilla", "Standard", "Stock", "Base", "Normal", "Default",  // EN/universal
    "Plate", "Tactical", "Helmet", "Carrier", "Armor", "Armour",  // EN item types
    "Plattenträger", "Helm", "Rüstung", "Schutz",                 // DE
    "Бронежилет", "Шлем", "Защита",                                // RU
    "Gilet", "Casque", "Blindage",                                 // FR
    NULL
};

// Words meaning "weapon" (for weapon slot detection)
static const char *STAT_WEAPON_WORDS[] = {
    "Weapon", "Gun", "Firearm",    // EN
    "Waffe", "Gewehr",             // DE
    "Оружие", "Оружия",           // RU
    "Arme", "Fusil",               // FR
    NULL
};

// Phrases meaning "high capacity" (storage vest, not armor)
static const char *STAT_HIGH_CAP_PHRASES[] = {
    "High Capacity", "Large Capacity", "Extra Storage",  // EN
    "Hohe Kapazität", "Große Kapazität",                  // DE (UTF-8)
    "Большая вместимость", "Увеличенная ёмкость",         // RU
    "Grande Capacité",                                    // FR
    NULL
};

// Check if haystack contains ANY of the NULL-terminated keyword list (case-insensitive).
static bool stat_contains_any(const char *haystack, const char *keywords[]) {
    if (!haystack) return false;
    for (int i = 0; keywords[i]; i++) {
        if (util_str_contains_ci(haystack, keywords[i])) return true;
    }
    return false;
}

// Match a line against item stats patterns.  Updates the stats of the matching
// WebItem if found.  Patterns (multi-language):
//   - "(Nx <reference> ...)" or "(N.Nx <reference> ...)" → protection multiplier
//   - "(N <slot_word>)" or "(N <slot_word> + <weapon_word>)" → slot count + weapon flag
//   - "(<reference> ...)" without multiplier → protection_mult=1.0
//   - "(<high_cap_phrase> ...)" → flag as high-cap vest (0.8x)
static void parse_stat_line(const char *line, WebItem *items, int item_count) {
    if (!line || !items || item_count <= 0) return;

    // Look for patterns like "Name (Nx Vanilla...)" or "Name (N Slots...)"
    const char *paren = strchr(line, '(');
    if (!paren) return;

    // Extract the name portion before the parenthesis
    // Trim whitespace and numbering like "1. " or "2. "
    const char *name_start = line;
    while (*name_start && (*name_start == ' ' || *name_start == '\t'))
        name_start++;
    // Skip leading numbers like "1. ", "2. ", etc.
    if (isdigit((unsigned char)*name_start)) {
        const char *p = name_start;
        while (isdigit((unsigned char)*p)) p++;
        if (*p == '.' && *(p + 1) == ' ') name_start = p + 2;
    }

    // Get the text inside parentheses
    const char *paren_end = strchr(paren, ')');
    if (!paren_end) return;

    char inside[256] = {0};
    int plen = (int)(paren_end - paren - 1);
    if (plen <= 0 || plen >= (int)sizeof(inside)) return;
    memcpy(inside, paren + 1, plen);
    inside[plen] = '\0';

    // Try to match stat patterns
    float protection = 0.0f;
    int slots = 0;
    bool weapon_slot = false;
    bool has_stat = false;

    // Pattern: "N.Nx <reference> ..." or "Nx <reference> ..."
    // Works in any language — the multiplier format (2.5x) is universal
    {
        float mult = 0.0f;
        char rest[128] = {0};
        if (sscanf(inside, "%fx %127[^\n]", &mult, rest) >= 1 && mult > 0.0f) {
            // Check if the rest contains a reference item keyword in any language
            if (stat_contains_any(rest, STAT_REFERENCE_WORDS)) {
                protection = mult;
                has_stat = true;
            }
        }
    }

    // Pattern: "<reference_word> ..." without multiplier → 1.0x baseline
    if (!has_stat && stat_contains_any(inside, STAT_REFERENCE_WORDS)) {
        // Only match if it looks like a stat annotation (not just random text
        // that happens to contain "Plate" or "Helmet")
        // Require the reference word to be near the start (within first 20 chars)
        bool near_start = false;
        for (int i = 0; STAT_REFERENCE_WORDS[i]; i++) {
            const char *found = util_str_contains_ci(inside, STAT_REFERENCE_WORDS[i])
                ? strstr(inside, STAT_REFERENCE_WORDS[i]) : NULL;
            // Case-insensitive search — re-check with manual scan
            if (!found) {
                // Simple case-insensitive position check
                int klen = (int)strlen(STAT_REFERENCE_WORDS[i]);
                for (int j = 0; inside[j] && j < 20; j++) {
                    bool match = true;
                    for (int k = 0; k < klen && inside[j + k]; k++) {
                        if (tolower((unsigned char)inside[j + k]) !=
                            tolower((unsigned char)STAT_REFERENCE_WORDS[i][k])) {
                            match = false;
                            break;
                        }
                    }
                    if (match) { near_start = true; break; }
                }
            } else {
                if ((found - inside) < 20) near_start = true;
            }
            if (near_start) break;
        }
        if (near_start) {
            protection = 1.0f;
            has_stat = true;
        }
    }

    // Pattern: "<high_cap_phrase> ..." → 0.8x protection (storage vest, not armor)
    if (!has_stat && stat_contains_any(inside, STAT_HIGH_CAP_PHRASES)) {
        protection = 0.8f;
        has_stat = true;
    }

    // Pattern: "N <slot_word>" or "N <slot_word> + <weapon_word>"
    // Matches any language: "150 Slots", "150 Plätze", "150 Слотов", etc.
    {
        int s = 0;
        char rest_buf[128] = {0};
        if (sscanf(inside, "%d %127[^\n]", &s, rest_buf) >= 1 && s > 0) {
            // Check if the text after the number contains a slot keyword
            if (stat_contains_any(rest_buf, STAT_SLOT_WORDS) ||
                stat_contains_any(inside, STAT_SLOT_WORDS)) {
                slots = s;
                weapon_slot = stat_contains_any(inside, STAT_WEAPON_WORDS);
                has_stat = true;
            }
        }
    }

    if (!has_stat) return;

    // Match the line's item name to a WebItem
    // Extract name text between start and paren
    char item_name[128] = {0};
    int name_len = (int)(paren - name_start);
    while (name_len > 0 && (name_start[name_len - 1] == ' ' ||
                            name_start[name_len - 1] == '\t'))
        name_len--;
    if (name_len <= 0 || name_len >= (int)sizeof(item_name)) return;
    memcpy(item_name, name_start, name_len);
    item_name[name_len] = '\0';

    // Fuzzy match: check if any WebItem classname contains words from item_name
    // e.g. "MK-V Vest" matches "MMG_MKV_Vest" or "MMG_MK_V_PlateCarrierVest"
    // Convert item_name words to search tokens
    for (int i = 0; i < item_count; i++) {
        WebItem *wi = &items[i];
        // Skip items that already have higher stats
        if (wi->stats.protection_mult > protection && protection > 0) continue;
        if (wi->stats.slot_count > slots && slots > 0) continue;

        // Try fuzzy match: each significant word in item_name appears in classname
        // Tokenize by space/dash and check each word
        char name_copy[128];
        strncpy(name_copy, item_name, sizeof(name_copy) - 1);
        name_copy[sizeof(name_copy) - 1] = '\0';

        bool all_match = true;
        int word_count = 0;
        char *tok_ctx = NULL;
        char *word = strtok_r(name_copy, " -", &tok_ctx);
        while (word) {
            if (strlen(word) >= 2) { // Skip very short words
                // Replace dashes in classname matching (MK-V → MKV or MK_V)
                if (!util_str_contains_ci(wi->classname, word)) {
                    all_match = false;
                    break;
                }
                word_count++;
            }
            word = strtok_r(NULL, " -", &tok_ctx);
        }

        if (all_match && word_count >= 1) {
            if (protection > 0.0f) {
                wi->stats.protection_mult = protection;
                strncpy(wi->stats.stat_source, "workshop_desc",
                        sizeof(wi->stats.stat_source) - 1);
            }
            if (slots > 0) {
                wi->stats.slot_count = slots;
                wi->stats.has_weapon_slot = weapon_slot;
                strncpy(wi->stats.stat_source, "workshop_desc",
                        sizeof(wi->stats.stat_source) - 1);
            }
            break; // First match wins
        }
    }
}

// Parse the full plain-text description for item stats.
// Called after classname extraction to annotate items with stats.
void web_extract_item_stats_from_description(ModWebInfo *mod,
                                             const char *plain_text) {
    if (!mod || !plain_text || mod->item_count <= 0) return;

    // Process line by line
    const char *line_start = plain_text;
    while (*line_start) {
        const char *line_end = strchr(line_start, '\n');
        if (!line_end) line_end = line_start + strlen(line_start);

        int line_len = (int)(line_end - line_start);
        if (line_len > 0 && line_len < 512) {
            char line[512];
            memcpy(line, line_start, line_len);
            line[line_len] = '\0';

            // Only parse lines that contain parentheses (stat annotations)
            if (strchr(line, '(')) {
                parse_stat_line(line, mod->items, mod->item_count);
            }
        }

        if (*line_end == '\0') break;
        line_start = line_end + 1;
    }

    // Check if any item got stats
    for (int i = 0; i < mod->item_count; i++) {
        if (mod->items[i].stats.protection_mult > 0.0f ||
            mod->items[i].stats.slot_count > 0) {
            mod->has_stats = true;
            break;
        }
    }
}

// ============================================================================
// INTERNAL: DISCUSSION LINK EXTRACTION
// ============================================================================
// Scans plain text for URLs that look like types.xml file links:
//   - pastebin.com/...
//   - hastebin.com/...
//   - paste.ee/...
//   - raw.githubusercontent.com/...
//   - Any URL containing "types" and ".xml"
//   - Google Drive / Dropbox links with "types" in context

static bool is_types_link(const char *url) {
    if (!url) return false;
    if (util_str_contains_ci(url, "pastebin.com"))     return true;
    if (util_str_contains_ci(url, "hastebin.com"))     return true;
    if (util_str_contains_ci(url, "paste.ee"))         return true;
    if (util_str_contains_ci(url, "paste2.org"))       return true;
    if (util_str_contains_ci(url, "dpaste.org"))       return true;
    if (util_str_contains_ci(url, "gist.github.com"))  return true;
    if (util_str_contains_ci(url, "raw.githubusercontent.com")) return true;
    if (util_str_contains_ci(url, "types") &&
        util_str_contains_ci(url, ".xml"))             return true;
    return false;
}

// Extract URLs from text.  Finds http:// and https:// links.
static int extract_urls(const char *text, char urls[][512], int max_urls) {
    int count = 0;
    const char *p = text;

    while (*p && count < max_urls) {
        const char *http = strstr(p, "http");
        if (!http) break;

        // Verify it's http:// or https://
        if (strncmp(http, "http://", 7) != 0 && strncmp(http, "https://", 8) != 0) {
            p = http + 4;
            continue;
        }

        // Extract URL (ends at whitespace, quote, ], ), or end of string)
        int len = 0;
        const char *u = http;
        while (*u && *u != ' ' && *u != '\t' && *u != '\n' && *u != '\r' &&
               *u != '"' && *u != '\'' && *u != ']' && *u != ')' &&
               *u != '>' && *u != '[' && len < 510) {
            len++;
            u++;
        }

        if (len >= 10 && len < 512) { // Minimum viable URL length
            memcpy(urls[count], http, len);
            urls[count][len] = '\0';

            // Dedup
            bool dup = false;
            for (int d = 0; d < count && !dup; d++) {
                if (strcmp(urls[d], urls[count]) == 0) dup = true;
            }
            if (!dup) count++;
        }

        p = http + len;
    }
    return count;
}

// Scan description for types.xml links and populate discussion_links.
static void extract_discussion_links(ModWebInfo *mod, const char *plain_text) {
    if (!mod || !plain_text) return;

    char urls[WEB_MAX_DISCUSSION_LINKS][512];
    int url_count = extract_urls(plain_text, urls, WEB_MAX_DISCUSSION_LINKS);

    for (int i = 0; i < url_count && mod->discussion_link_count < WEB_MAX_DISCUSSION_LINKS; i++) {
        if (is_types_link(urls[i])) {
            DiscussionLink *link = &mod->discussion_links[mod->discussion_link_count];
            memset(link, 0, sizeof(DiscussionLink));
            strncpy(link->url, urls[i], sizeof(link->url) - 1);
            strncpy(link->poster_name, "mod_author", sizeof(link->poster_name) - 1);
            link->is_author = true; // Description links are always from the author
            mod->discussion_link_count++;

            util_log(SEVERITY_INFO, "WebLookup: Found types link in %s: %s",
                     mod->mod_name, urls[i]);
        }
    }
}

// ============================================================================
// INTERNAL: STAT-BASED TIER PROMOTION
// ============================================================================
// Uses item stats to determine if an item deserves a higher tier than its
// classname-based assignment.

int web_stat_tier_promotion(const ItemStats *stats) {
    if (!stats) return 0;

    int tier = 0;

    // Protection-based promotion
    if (stats->protection_mult >= 2.0f)      tier = 8;  // Operator (endgame armor)
    else if (stats->protection_mult >= 1.5f) tier = 7;  // Spec-Ops (upgraded armor)
    else if (stats->protection_mult >= 1.0f) tier = 6;  // Infantry (vanilla-equivalent)

    // Slot-based promotion (backpacks, storage)
    if (stats->slot_count >= 150)      { if (tier < 7) tier = 7; }  // Huge backpack
    else if (stats->slot_count >= 120) { if (tier < 7) tier = 7; }  // Large backpack
    else if (stats->slot_count >= 80)  { if (tier < 6) tier = 6; }  // Military backpack

    // Weapon slot bonus: +1 tier (versatile backpack)
    if (stats->has_weapon_slot && tier > 0 && tier < 9) {
        tier++;
    }

    return tier;
}

// ============================================================================
// INTERNAL: CACHE SYSTEM
// ============================================================================
// One file per mod in config/web_cache/:
//   WORKSHOP=<id>
//   MOD=<folder name>
//   TITLE=<Workshop title>
//   FETCHED=<unix timestamp>
//   ITEMS=<count>
//   ITEM=<classname>|<category>|<parent_hint>

// Build safe cache filename from mod name (replace spaces → underscores).
static void cache_path_for_mod(const char *cache_dir, const char *mod_name,
                               char *path, int max_len) {
    char safe[128];
    int j = 0;
    for (int i = 0; mod_name[i] && j < 126; i++) {
        char c = mod_name[i];
        if (isalnum((unsigned char)c) || c == '_' || c == '-' || c == '@')
            safe[j++] = c;
        else if (c == ' ')
            safe[j++] = '_';
    }
    safe[j] = '\0';
    snprintf(path, max_len, "%s/%s.cache", cache_dir, safe);
}

static bool write_mod_cache(const char *cache_dir, const ModWebInfo *mod) {
    char path[512];
    cache_path_for_mod(cache_dir, mod->mod_name, path, sizeof(path));

    FILE *f = fopen(path, "w");
    if (!f) {
        util_log(SEVERITY_WARNING, "WebLookup: Failed to write cache: %s", path);
        return false;
    }

    fprintf(f, "WORKSHOP=%llu\n", mod->workshop_id);
    fprintf(f, "MOD=%s\n", mod->mod_name);
    fprintf(f, "TITLE=%s\n", mod->title);
    fprintf(f, "CREATOR=%llu\n", mod->creator_id);
    fprintf(f, "FETCHED=%lld\n", (long long)mod->fetched_at);
    fprintf(f, "HAS_STATS=%d\n", mod->has_stats ? 1 : 0);
    fprintf(f, "ITEMS=%d\n", mod->item_count);
    for (int i = 0; i < mod->item_count; i++) {
        const WebItem *item = &mod->items[i];
        fprintf(f, "ITEM=%s|%s|%s|%.2f|%d|%d|%s\n",
                item->classname,
                item->category,
                item->parent_hint,
                item->stats.protection_mult,
                item->stats.slot_count,
                item->stats.has_weapon_slot ? 1 : 0,
                item->stats.stat_source);
    }
    // Write discussion links
    for (int i = 0; i < mod->discussion_link_count; i++) {
        const DiscussionLink *link = &mod->discussion_links[i];
        fprintf(f, "LINK=%s|%s|%d\n",
                link->url, link->poster_name, link->is_author ? 1 : 0);
    }
    fclose(f);
    return true;
}

static bool read_mod_cache(const char *path, ModWebInfo *mod) {
    FILE *f = fopen(path, "r");
    if (!f) return false;

    memset(mod, 0, sizeof(*mod));
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';

        if (strncmp(line, "WORKSHOP=", 9) == 0) {
            mod->workshop_id = strtoull(line + 9, NULL, 10);
        } else if (strncmp(line, "MOD=", 4) == 0) {
            strncpy(mod->mod_name, line + 4, sizeof(mod->mod_name) - 1);
        } else if (strncmp(line, "TITLE=", 6) == 0) {
            strncpy(mod->title, line + 6, sizeof(mod->title) - 1);
        } else if (strncmp(line, "CREATOR=", 8) == 0) {
            mod->creator_id = strtoull(line + 8, NULL, 10);
        } else if (strncmp(line, "FETCHED=", 8) == 0) {
            mod->fetched_at = (time_t)strtoll(line + 8, NULL, 10);
        } else if (strncmp(line, "HAS_STATS=", 10) == 0) {
            mod->has_stats = (atoi(line + 10) != 0);
        } else if (strncmp(line, "ITEM=", 5) == 0 &&
                   mod->item_count < WEB_MAX_ITEMS_PER_MOD) {
            // Parse ITEM=classname|category|parent_hint|protection|slots|weapon|source
            char buf[1024];
            strncpy(buf, line + 5, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';

            // Split by pipe — handle both old (3-field) and new (7-field) format
            char *fields[7] = {0};
            int field_count = 0;
            char *p = buf;
            fields[0] = p;
            field_count = 1;
            while (*p && field_count < 7) {
                if (*p == '|') {
                    *p = '\0';
                    fields[field_count++] = p + 1;
                }
                p++;
            }

            if (field_count < 3) continue; // Need at least classname|category|parent

            WebItem *item = &mod->items[mod->item_count];
            memset(item, 0, sizeof(WebItem));
            strncpy(item->classname, fields[0], sizeof(item->classname) - 1);
            strncpy(item->category, fields[1], sizeof(item->category) - 1);
            strncpy(item->parent_hint, fields[2], sizeof(item->parent_hint) - 1);

            // Parse extended stats (new format)
            if (field_count >= 7) {
                item->stats.protection_mult = (float)atof(fields[3]);
                item->stats.slot_count = atoi(fields[4]);
                item->stats.has_weapon_slot = (atoi(fields[5]) != 0);
                strncpy(item->stats.stat_source, fields[6],
                        sizeof(item->stats.stat_source) - 1);
            }
            mod->item_count++;
        } else if (strncmp(line, "LINK=", 5) == 0 &&
                   mod->discussion_link_count < WEB_MAX_DISCUSSION_LINKS) {
            // Parse LINK=url|poster_name|is_author
            char buf[1024];
            strncpy(buf, line + 5, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';

            char *sep1 = strchr(buf, '|');
            if (!sep1) continue;
            *sep1 = '\0';
            char *sep2 = strchr(sep1 + 1, '|');
            if (!sep2) continue;
            *sep2 = '\0';

            DiscussionLink *link = &mod->discussion_links[mod->discussion_link_count];
            memset(link, 0, sizeof(DiscussionLink));
            strncpy(link->url, buf, sizeof(link->url) - 1);
            strncpy(link->poster_name, sep1 + 1, sizeof(link->poster_name) - 1);
            link->is_author = (atoi(sep2 + 1) != 0);
            mod->discussion_link_count++;
        }
    }
    fclose(f);

    mod->valid = (mod->workshop_id > 0 && mod->mod_name[0]);
    return mod->valid;
}

// ============================================================================
// INTERNAL: RELATIONSHIP INFERENCE
// ============================================================================
// After extracting classnames, try to infer which items are attachments for
// which base items.  Uses the same mod-prefix heuristic as the weapon system:
// if two items share the first segment before '_', they're from the same mod
// and likely compatible.

static void infer_parent_relationships(ModWebInfo *mod) {
    for (int a = 0; a < mod->item_count; a++) {
        if (strcmp(mod->items[a].category, "attachment") != 0 &&
            strcmp(mod->items[a].category, "equip_attach") != 0)
            continue;
        if (mod->items[a].parent_hint[0]) continue; // Already set

        const char *att_cn = mod->items[a].classname;
        const char *att_u = strchr(att_cn, '_');
        if (!att_u || att_u == att_cn) continue;
        int att_plen = (int)(att_u - att_cn);

        for (int w = 0; w < mod->item_count; w++) {
            if (a == w) continue;
            if (strcmp(mod->items[w].category, "weapon") != 0 &&
                strcmp(mod->items[w].category, "equipment") != 0)
                continue;

            const char *base_cn = mod->items[w].classname;
            const char *base_u = strchr(base_cn, '_');
            if (!base_u || base_u == base_cn) continue;
            int base_plen = (int)(base_u - base_cn);

            if (att_plen != base_plen) continue;

            bool match = true;
            for (int c = 0; c < att_plen; c++) {
                if (tolower((unsigned char)att_cn[c]) !=
                    tolower((unsigned char)base_cn[c])) {
                    match = false;
                    break;
                }
            }
            if (match) {
                strncpy(mod->items[a].parent_hint, base_cn,
                        sizeof(mod->items[a].parent_hint) - 1);
                break; // First match wins
            }
        }
    }
}

// ============================================================================
// PUBLIC API
// ============================================================================

bool web_lookup_init(WebLookupState *state, const char *cache_dir) {
    if (!state) return false;
    memset(state, 0, sizeof(WebLookupState));
    strncpy(state->cache_dir, cache_dir, sizeof(state->cache_dir) - 1);

#ifdef _WIN32
    state->h_internet = (void *)InternetOpenA(
        "StelliferumAuditor/2.0",
        INTERNET_OPEN_TYPE_PRECONFIG,
        NULL, NULL, 0);
    if (!state->h_internet) {
        util_log(SEVERITY_ERROR,
                 "WebLookup: InternetOpen failed (error=%lu)", GetLastError());
        return false;
    }

    // Session-level timeouts
    DWORD timeout = 15000;
    InternetSetOptionA((HINTERNET)state->h_internet,
                       INTERNET_OPTION_CONNECT_TIMEOUT,
                       &timeout, sizeof(timeout));
    timeout = 30000;
    InternetSetOptionA((HINTERNET)state->h_internet,
                       INTERNET_OPTION_RECEIVE_TIMEOUT,
                       &timeout, sizeof(timeout));
#endif

    // Ensure cache directory exists
    util_ensure_directory(cache_dir);

    state->initialized = true;
    util_log(SEVERITY_INFO, "WebLookup: Initialized (cache: %s)", cache_dir);
    return true;
}

void web_lookup_shutdown(WebLookupState *state) {
    if (!state) return;

#ifdef _WIN32
    if (state->h_internet) {
        InternetCloseHandle((HINTERNET)state->h_internet);
        state->h_internet = NULL;
    }
#endif

    util_log(SEVERITY_INFO,
             "WebLookup: Shutdown — %d fetches, %d cache hits, %d mods loaded",
             state->fetch_count, state->cache_hits, state->mod_count);
    state->initialized = false;
}

bool web_fetch_workshop_mod(WebLookupState *state,
                            unsigned long long workshop_id,
                            const char *mod_name) {
    if (!state || !state->initialized || workshop_id == 0) return false;
    if (state->mod_count >= WEB_MAX_MODS) {
        util_log(SEVERITY_WARNING,
                 "WebLookup: Max mods (%d) reached, skipping %s",
                 WEB_MAX_MODS, mod_name ? mod_name : "(null)");
        return false;
    }

    // Already loaded?
    for (int i = 0; i < state->mod_count; i++) {
        if (state->mods[i].workshop_id == workshop_id) {
            util_log(SEVERITY_INFO, "WebLookup: %s already loaded", mod_name);
            return true;
        }
    }

    ModWebInfo *mod = &state->mods[state->mod_count];
    memset(mod, 0, sizeof(ModWebInfo));
    mod->workshop_id = workshop_id;
    if (mod_name)
        strncpy(mod->mod_name, mod_name, sizeof(mod->mod_name) - 1);

    // ── Check cache ──────────────────────────────────────────────────────────
    char cache_path[512];
    cache_path_for_mod(state->cache_dir, mod_name ? mod_name : "unknown",
                       cache_path, sizeof(cache_path));
    if (read_mod_cache(cache_path, mod)) {
        time_t age = time(NULL) - mod->fetched_at;
        if (age >= 0 && age < WEB_CACHE_MAX_AGE_SECS) {
            state->cache_hits++;
            state->mod_count++;
            util_log(SEVERITY_INFO,
                     "WebLookup: Cache hit for %s (%d items, age %llds)",
                     mod_name, mod->item_count, (long long)age);
            return true;
        }
        util_log(SEVERITY_INFO,
                 "WebLookup: Cache stale for %s (age %llds), re-fetching",
                 mod_name, (long long)age);
        memset(mod, 0, sizeof(ModWebInfo));
        mod->workshop_id = workshop_id;
        if (mod_name)
            strncpy(mod->mod_name, mod_name, sizeof(mod->mod_name) - 1);
    }

    // ── Fetch from Steam Workshop API ────────────────────────────────────────
    // URL-encode [0] as %5B0%5D
    char post_data[256];
    snprintf(post_data, sizeof(post_data),
             "itemcount=1&publishedfileids%%5B0%%5D=%llu", workshop_id);

    char *response = (char *)malloc(WEB_MAX_RESPONSE);
    if (!response) return false;

    util_log(SEVERITY_INFO,
             "WebLookup: Fetching Workshop ID %llu (%s)...",
             workshop_id, mod_name ? mod_name : "unknown");

    int bytes = http_post(state->h_internet,
                          "api.steampowered.com",
                          "/ISteamRemoteStorage/GetPublishedFileDetails/v1/",
                          post_data, response, WEB_MAX_RESPONSE);
    state->fetch_count++;

    if (bytes <= 0) {
        util_log(SEVERITY_WARNING,
                 "WebLookup: Empty response for Workshop ID %llu", workshop_id);
        free(response);
        return false;
    }

    // Extract title
    json_extract_string(response, "title", mod->title, sizeof(mod->title));

    // Extract creator Steam ID (for discussion post author verification)
    mod->creator_id = json_extract_ull(response, "creator");

    // Extract description (BBCode text with item lists)
    char *description = (char *)malloc(WEB_MAX_DESCRIPTION);
    if (!description) { free(response); return false; }

    int desc_len = json_extract_string(response, "description",
                                       description, WEB_MAX_DESCRIPTION);
    free(response);

    if (desc_len <= 0) {
        util_log(SEVERITY_WARNING,
                 "WebLookup: No description in Workshop ID %llu", workshop_id);
        free(description);
        return false;
    }

    // Strip BBCode → plain text
    char *plain = (char *)malloc(WEB_MAX_DESCRIPTION);
    if (!plain) { free(description); return false; }
    strip_bbcode(description, plain, WEB_MAX_DESCRIPTION);
    free(description);

    // Extract classname-like tokens
    // Use a temporary static-ish buffer (stack is fine for 512×128 = 64 KB)
    char (*classnames)[128] = (char (*)[128])malloc(WEB_MAX_ITEMS_PER_MOD * 128);
    if (!classnames) { free(plain); return false; }

    int cn_count = extract_classnames(plain, classnames, WEB_MAX_ITEMS_PER_MOD);

    // Classify and store
    for (int i = 0; i < cn_count && mod->item_count < WEB_MAX_ITEMS_PER_MOD; i++) {
        WebItem *item = &mod->items[mod->item_count];
        memset(item, 0, sizeof(WebItem));
        strncpy(item->classname, classnames[i], sizeof(item->classname) - 1);
        strncpy(item->category,
                classify_web_classname(classnames[i]),
                sizeof(item->category) - 1);
        item->parent_hint[0] = '\0';
        mod->item_count++;
    }
    free(classnames);

    // Infer attachment → base-item relationships
    infer_parent_relationships(mod);

    // Extract item stats (protection multipliers, slot counts) from description
    web_extract_item_stats_from_description(mod, plain);

    // Extract discussion links (types.xml URLs from pastebin, etc.)
    extract_discussion_links(mod, plain);

    free(plain);

    mod->fetched_at = time(NULL);
    mod->valid = true;
    state->mod_count++;

    // Persist to cache
    write_mod_cache(state->cache_dir, mod);

    util_log(SEVERITY_INFO,
             "WebLookup: %s — %d classnames, %d stat-annotated, %d discussion links from \"%s\"",
             mod_name, mod->item_count,
             mod->has_stats ? mod->item_count : 0,
             mod->discussion_link_count,
             mod->title);
    return true;
}

bool web_fetch_wiki_page(WebLookupState *state, const char *item_name,
                         char *text_out, int max_len) {
    if (!state || !state->initialized || !item_name || !text_out) return false;

    // Build Fandom MediaWiki API URL (spaces → underscores)
    char page_name[256];
    int j = 0;
    for (int i = 0; item_name[i] && j < 254; i++)
        page_name[j++] = (item_name[i] == ' ') ? '_' : item_name[i];
    page_name[j] = '\0';

    char url[512];
    snprintf(url, sizeof(url),
             "https://dayz.fandom.com/api.php"
             "?action=parse&page=%s&format=json&prop=wikitext",
             page_name);

    char *response = (char *)malloc(WEB_MAX_RESPONSE);
    if (!response) return false;

    int bytes = http_get(state->h_internet, url, response, WEB_MAX_RESPONSE);
    state->fetch_count++;

    if (bytes <= 0) {
        util_log(SEVERITY_WARNING,
                 "WebLookup: Wiki fetch failed for '%s'", item_name);
        free(response);
        return false;
    }

    // The wikitext is nested: parse.wikitext["*"]
    // json_extract_string finds the first "*" key — which is the wikitext
    int len = json_extract_string(response, "*", text_out, max_len);
    free(response);
    return (len > 0);
}

int web_load_workshop_ids(const char *ini_path,
                          char mod_names[][128],
                          unsigned long long *ids,
                          int max_entries) {
    FILE *f = fopen(ini_path, "r");
    if (!f) return 0;

    int count = 0;
    char line[512];
    while (fgets(line, sizeof(line), f) && count < max_entries) {
        line[strcspn(line, "\r\n")] = '\0';

        // Skip comments, empty lines, section headers
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ';' || *p == '#' || *p == '\0' || *p == '[') continue;

        // Parse:  @ModName = WorkshopID
        char *eq = strchr(p, '=');
        if (!eq) continue;

        // Trim mod name (left of '=')
        char *name_end = eq - 1;
        while (name_end > p && (*name_end == ' ' || *name_end == '\t'))
            name_end--;
        *(name_end + 1) = '\0';

        // Trim workshop ID (right of '=')
        char *id_start = eq + 1;
        while (*id_start == ' ' || *id_start == '\t') id_start++;

        unsigned long long wid = strtoull(id_start, NULL, 10);
        if (wid == 0) continue;

        strncpy(mod_names[count], p, 127);
        mod_names[count][127] = '\0';
        ids[count] = wid;
        count++;
    }
    fclose(f);
    return count;
}

bool web_load_cache(WebLookupState *state) {
    if (!state) return false;

#ifdef _WIN32
    char pattern[512];
    snprintf(pattern, sizeof(pattern), "%s\\*.cache", state->cache_dir);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return false;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (state->mod_count >= WEB_MAX_MODS) break;

        char path[512];
        snprintf(path, sizeof(path), "%s\\%s", state->cache_dir, fd.cFileName);

        ModWebInfo *mod = &state->mods[state->mod_count];
        if (read_mod_cache(path, mod)) {
            state->mod_count++;
            state->cache_hits++;
        }
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
#endif

    util_log(SEVERITY_INFO,
             "WebLookup: Loaded %d mods from cache", state->mod_count);
    return (state->mod_count > 0);
}

bool web_save_cache(WebLookupState *state) {
    if (!state) return false;

    int saved = 0;
    for (int i = 0; i < state->mod_count; i++) {
        if (state->mods[i].valid &&
            write_mod_cache(state->cache_dir, &state->mods[i]))
            saved++;
    }
    util_log(SEVERITY_INFO, "WebLookup: Saved %d mod caches", saved);
    return (saved > 0);
}

bool web_fetch_all_configured(WebLookupState *state,
                              const char *workshop_ids_path) {
    if (!state || !state->initialized) return false;

    char mod_names[WEB_MAX_MODS][128];
    unsigned long long ids[WEB_MAX_MODS];
    int count = web_load_workshop_ids(workshop_ids_path, mod_names, ids,
                                      WEB_MAX_MODS);
    if (count == 0) {
        util_log(SEVERITY_INFO,
                 "WebLookup: No Workshop IDs in %s — "
                 "add @ModFolder = WorkshopID entries to enable web lookup",
                 workshop_ids_path);
        return false;
    }

    util_log(SEVERITY_INFO,
             "WebLookup: Processing %d configured mods...", count);

    int success = 0;
    for (int i = 0; i < count; i++) {
        if (web_fetch_workshop_mod(state, ids[i], mod_names[i]))
            success++;
    }

    util_log(SEVERITY_INFO,
             "WebLookup: %d/%d mods loaded (fetched: %d, cached: %d)",
             success, count, state->fetch_count, state->cache_hits);
    return (success > 0);
}

int web_find_attachments_for(const WebLookupState *state,
                             const char *base_classname,
                             WebItem *out_items, int max_out) {
    if (!state || !base_classname || !out_items || max_out <= 0) return 0;

    int count = 0;

    for (int m = 0; m < state->mod_count && count < max_out; m++) {
        const ModWebInfo *mod = &state->mods[m];
        if (!mod->valid) continue;

        for (int i = 0; i < mod->item_count && count < max_out; i++) {
            const WebItem *web = &mod->items[i];

            // Only return items classified as attachments
            if (strcmp(web->category, "attachment") != 0 &&
                strcmp(web->category, "equip_attach") != 0)
                continue;

            bool match = false;

            // 1. Explicit parent match (inferred during fetch)
            if (web->parent_hint[0] &&
                util_str_contains_ci(base_classname, web->parent_hint))
                match = true;

            // 2. Shared mod prefix (first segment before '_')
            if (!match) {
                const char *bu = strchr(base_classname, '_');
                const char *au = strchr(web->classname, '_');
                if (bu && au) {
                    int blen = (int)(bu - base_classname);
                    int alen = (int)(au - web->classname);
                    if (blen == alen && blen > 1) {
                        bool pfx = true;
                        for (int c = 0; c < blen; c++) {
                            if (tolower((unsigned char)base_classname[c]) !=
                                tolower((unsigned char)web->classname[c])) {
                                pfx = false;
                                break;
                            }
                        }
                        if (pfx) match = true;
                    }
                }
            }

            // 3. Base classname substring in attachment name
            if (!match && strlen(base_classname) >= 4 &&
                util_str_contains_ci(web->classname, base_classname))
                match = true;

            if (match) {
                // Dedup
                bool dup = false;
                for (int d = 0; d < count && !dup; d++) {
                    if (util_strcasecmp(out_items[d].classname,
                                        web->classname) == 0)
                        dup = true;
                }
                if (!dup)
                    out_items[count++] = *web;
            }
        }
    }
    return count;
}

bool web_item_exists(const WebLookupState *state, const char *classname) {
    if (!state || !classname) return false;

    for (int m = 0; m < state->mod_count; m++) {
        if (!state->mods[m].valid) continue;
        for (int i = 0; i < state->mods[m].item_count; i++) {
            if (util_strcasecmp(state->mods[m].items[i].classname,
                                classname) == 0)
                return true;
        }
    }
    return false;
}

// ============================================================================
// PUBLIC: ITEM STAT LOOKUP
// ============================================================================

bool web_get_item_stats(const WebLookupState *state,
                        const char *classname,
                        ItemStats *out_stats) {
    if (!state || !classname || !out_stats) return false;
    memset(out_stats, 0, sizeof(ItemStats));

    for (int m = 0; m < state->mod_count; m++) {
        if (!state->mods[m].valid || !state->mods[m].has_stats) continue;
        for (int i = 0; i < state->mods[m].item_count; i++) {
            const WebItem *item = &state->mods[m].items[i];
            if (util_strcasecmp(item->classname, classname) == 0) {
                if (item->stats.protection_mult > 0.0f ||
                    item->stats.slot_count > 0) {
                    *out_stats = item->stats;
                    return true;
                }
            }
        }
    }

    // Fuzzy match: try partial classname matching for modded items
    // e.g. "MMG_MKV_PlateCarrierVest_Black" should match stats for MK-V Vest
    for (int m = 0; m < state->mod_count; m++) {
        if (!state->mods[m].valid || !state->mods[m].has_stats) continue;
        for (int i = 0; i < state->mods[m].item_count; i++) {
            const WebItem *item = &state->mods[m].items[i];
            if (item->stats.protection_mult <= 0.0f &&
                item->stats.slot_count <= 0) continue;

            // Check if classname contains the web item classname
            if (util_str_contains_ci(classname, item->classname) ||
                util_str_contains_ci(item->classname, classname)) {
                *out_stats = item->stats;
                return true;
            }
        }
    }

    return false;
}
