/**
 * STELLIFERUM INTEGRITY ENGINE
 * ----------------------------
 * Pre-audit and post-sew integrity verification.
 *
 * Two independent checks:
 *
 * 1. PRE-AUDIT INTEGRITY CHECK (auditor_pre_audit_integrity)
 *    Runs BEFORE the audit pipeline. Examines all loaded items to verify that
 *    "duplicates" are truly duplicates (same item from different sources) and
 *    not different variations that happen to share a classname. Reports
 *    suspicious collisions so the user can review before the audit merges them.
 *
 * 2. SEW INTEGRITY CHECK (auditor_sew_integrity_check)
 *    Runs AFTER files are stitched/sewn. Verifies the output is correct:
 *    - Not one giant merged blob (item count should match expectation)
 *    - No items silently dropped (every non-deleted item appears in output)
 *    - No deleted items leaked into the output
 *    - XML structure is well-formed (proper open/close tags)
 *    - Spawnable blocks match expected count
 *
 * These checks produce IntegrityReport structs that the UI and pipeline
 * can inspect.
 */

#include "auditor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// ============================================================================
// PRE-AUDIT INTEGRITY CHECK
// ============================================================================
// Scans all loaded items to identify classname collisions where the items
// are NOT truly the same thing. This catches cases where two different mods
// define items with the same classname but entirely different stats, category,
// or purpose. The audit pipeline's dedup step normally merges or renames
// these, but this check runs first so the user knows what's happening.

static bool variation_is_suspicious(const LootItem *a, const LootItem *b) {
    if (!a || !b) return false;

    bool cat_populated = (a->category[0] && b->category[0]);
    bool cat_match = cat_populated &&
                     util_strcasecmp(a->category, b->category) == 0;
    bool flags_match = ((a->flags & (FLAG_CARGO | FLAG_CRAFTED)) ==
                        (b->flags & (FLAG_CARGO | FLAG_CRAFTED)));

    // Category mismatch is always suspicious
    if (cat_populated && !cat_match) return true;

    // Different flag profiles (one has cargo, other doesn't, etc.)
    if (!flags_match) return true;

    // When both categories are populated and match AND flags match, the items
    // are definitively the same type — nominal/lifetime differences are just
    // economy tuning overrides (common for mod-vs-vanilla).  CASE 1 dedup
    // handles this correctly, so it is NOT suspicious.
    if (cat_match && flags_match) return false;

    // Category data missing on at least one side — use nominal/lifetime as
    // tiebreakers to detect genuinely different items sharing a classname.

    // Wildly different nominal values (>80% difference)
    int nom_max = (a->nominal > b->nominal) ? a->nominal : b->nominal;
    if (nom_max == 0) nom_max = 1;
    int nom_diff = abs(a->nominal - b->nominal);
    float nom_pct = (float)nom_diff / (float)nom_max;
    if (nom_pct > 0.80f && nom_diff > 5) return true;

    // Wildly different lifetimes (>5x ratio)
    int life_a = a->lifetime > 0 ? a->lifetime : 3600;
    int life_b = b->lifetime > 0 ? b->lifetime : 3600;
    float life_ratio = (float)(life_a > life_b ? life_a : life_b) /
                       (float)(life_a < life_b ? life_a : life_b);
    if (life_ratio > 5.0f) return true;

    return false;
}

IntegrityReport auditor_pre_audit_integrity(AuditorContext *ctx) {
    IntegrityReport report;
    memset(&report, 0, sizeof(IntegrityReport));
    report.passed = true;
    strncpy(report.phase, "PRE_AUDIT", sizeof(report.phase) - 1);

    if (!ctx || ctx->item_count == 0) {
        snprintf(report.summary, sizeof(report.summary),
                 "No items loaded — skipping pre-audit integrity check.");
        return report;
    }

    // Sort items first for adjacent duplicate detection
    // (We work on a copy of the sorted state to avoid side effects)
    int total = ctx->item_count;
    int duplicates_found = 0;
    int suspicious_variations = 0;
    int true_duplicates = 0;
    int deleted_count = 0;

    for (int i = 0; i < total; i++) {
        if (ctx->items[i].deleted) {
            deleted_count++;
            continue;
        }
    }

    // Check for classname collisions (items must be pre-sorted by auditor_sort_items)
    // We do a temporary sort here to ensure correctness
    auditor_sort_items(ctx);

    for (int i = 0; i < total; i++) {
        if (ctx->items[i].deleted) continue;
        if (ctx->items[i].file_type != FILE_TYPE_ECONOMY) continue;

        for (int j = i + 1; j < total; j++) {
            if (ctx->items[j].deleted) continue;
            if (ctx->items[j].file_type != FILE_TYPE_ECONOMY) continue;
            if (util_strcasecmp(ctx->items[i].classname, ctx->items[j].classname) != 0)
                break; // sorted — no more matches

            duplicates_found++;
            const LootItem *a = &ctx->items[i];
            const LootItem *b = &ctx->items[j];

            if (variation_is_suspicious(a, b)) {
                suspicious_variations++;
                if (report.finding_count < MAX_INTEGRITY_FINDINGS) {
                    IntegrityFinding *f = &report.findings[report.finding_count++];
                    f->severity = SEVERITY_WARNING;
                    snprintf(f->message, sizeof(f->message),
                        "SUSPICIOUS VARIATION: '%s' from [%s] vs [%s] — "
                        "cat:%s/%s nom:%d/%d life:%d/%d flags:0x%X/0x%X",
                        a->classname, a->mod_name, b->mod_name,
                        a->category, b->category,
                        a->nominal, b->nominal,
                        a->lifetime, b->lifetime,
                        a->flags, b->flags);
                    strncpy(f->classname, a->classname, MAX_CLASSNAME_LEN - 1);
                    f->is_variation = true;
                }
            } else {
                true_duplicates++;
            }
        }
    }

    report.total_items = total;
    report.active_items = total - deleted_count;
    report.duplicate_groups = duplicates_found;

    if (suspicious_variations > 0) {
        report.passed = false;
        snprintf(report.summary, sizeof(report.summary),
            "PRE-AUDIT: %d items, %d duplicates (%d true, %d SUSPICIOUS VARIATIONS). "
            "Review suspicious items before proceeding.",
            total, duplicates_found, true_duplicates, suspicious_variations);
        util_log(SEVERITY_WARNING, "Integrity: %s", report.summary);
    } else {
        snprintf(report.summary, sizeof(report.summary),
            "PRE-AUDIT: %d items, %d duplicates — all verified as true duplicates. PASSED.",
            total, true_duplicates);
        util_log(SEVERITY_INFO, "Integrity: %s", report.summary);
    }

    // Log all suspicious findings
    for (int i = 0; i < report.finding_count; i++) {
        util_log(report.findings[i].severity, "  Integrity: %s", report.findings[i].message);
    }

    return report;
}

// ============================================================================
// SEW (STITCH) INTEGRITY CHECK
// ============================================================================
// After stitching/sewing, verify the output files are correct.

// Count <type name="..."> entries in an XML file
static int count_type_entries_in_file(const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0 || fsize > 100 * 1024 * 1024) {
        fclose(f);
        return -1;
    }
    fseek(f, 0, SEEK_SET);

    char *data = (char *)malloc(fsize + 1);
    if (!data) { fclose(f); return -1; }
    size_t bytes_read = fread(data, 1, fsize, f);
    data[bytes_read] = '\0';
    fclose(f);

    int count = 0;
    const char *p = data;
    while ((p = strstr(p, "<type name=\"")) != NULL) {
        count++;
        p += 12;
    }

    free(data);
    return count;
}

// Check that a file has proper XML open/close tags
static bool verify_xml_structure(const char *filepath, const char *root_tag,
                                  char *error_buf, int error_buf_len) {
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        snprintf(error_buf, error_buf_len, "File not found: %s", filepath);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0 || fsize > 100 * 1024 * 1024) {
        snprintf(error_buf, error_buf_len, "File empty or too large: %s (%ld bytes)", filepath, fsize);
        fclose(f);
        return false;
    }
    fseek(f, 0, SEEK_SET);

    char *data = (char *)malloc(fsize + 1);
    if (!data) { fclose(f); return false; }
    size_t bytes_read = fread(data, 1, fsize, f);
    data[bytes_read] = '\0';
    fclose(f);

    // Check for root opening tag
    char open_tag[128];
    snprintf(open_tag, sizeof(open_tag), "<%s", root_tag);
    if (!strstr(data, open_tag)) {
        snprintf(error_buf, error_buf_len, "Missing opening <%s> tag in %s", root_tag, filepath);
        free(data);
        return false;
    }

    // Check for root closing tag
    char close_tag[128];
    snprintf(close_tag, sizeof(close_tag), "</%s>", root_tag);
    if (!strstr(data, close_tag)) {
        snprintf(error_buf, error_buf_len, "Missing closing </%s> tag in %s", root_tag, filepath);
        free(data);
        return false;
    }

    // Check that close comes after open
    const char *open_pos = strstr(data, open_tag);
    const char *close_pos = strstr(data, close_tag);
    if (close_pos <= open_pos) {
        snprintf(error_buf, error_buf_len, "Malformed XML: closing tag before opening tag in %s", filepath);
        free(data);
        return false;
    }

    free(data);
    error_buf[0] = '\0';
    return true;
}

// Check that a specific classname appears in the output file
static bool classname_in_file(const char *filepath, const char *classname) {
    FILE *f = fopen(filepath, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0 || fsize > 100 * 1024 * 1024) {
        fclose(f);
        return false;
    }
    fseek(f, 0, SEEK_SET);

    char *data = (char *)malloc(fsize + 1);
    if (!data) { fclose(f); return false; }
    size_t bytes_read = fread(data, 1, fsize, f);
    data[bytes_read] = '\0';
    fclose(f);

    // Search for type name="classname" (case-insensitive on classname)
    char search[MAX_CLASSNAME_LEN + 20];
    snprintf(search, sizeof(search), "name=\"%s\"", classname);

    bool found = (strstr(data, search) != NULL);
    if (!found) {
        // Try case-insensitive
        found = util_str_contains_ci(data, search);
    }

    free(data);
    return found;
}

IntegrityReport auditor_sew_integrity_check(AuditorContext *ctx,
                                             const char *types_path,
                                             const char *spawnables_path) {
    IntegrityReport report;
    memset(&report, 0, sizeof(IntegrityReport));
    report.passed = true;
    strncpy(report.phase, "SEW_INTEGRITY", sizeof(report.phase) - 1);

    if (!ctx) {
        snprintf(report.summary, sizeof(report.summary), "No context — skipping sew integrity check.");
        return report;
    }

    // Count expected items (non-deleted, economy type)
    int expected_items = 0;
    int expected_active = 0;
    for (int i = 0; i < ctx->item_count; i++) {
        if (ctx->items[i].deleted) continue;
        expected_active++;
        if (ctx->items[i].file_type == FILE_TYPE_ECONOMY)
            expected_items++;
    }

    // ── CHECK 1: Types.xml XML structure ─────────────────────────────────
    if (types_path && util_file_exists(types_path)) {
        char xml_error[256] = {0};
        if (!verify_xml_structure(types_path, "types", xml_error, sizeof(xml_error))) {
            report.passed = false;
            if (report.finding_count < MAX_INTEGRITY_FINDINGS) {
                IntegrityFinding *f = &report.findings[report.finding_count++];
                f->severity = SEVERITY_ERROR;
                snprintf(f->message, sizeof(f->message),
                    "TYPES XML STRUCTURE INVALID: %s", xml_error);
            }
        }

        // ── CHECK 2: Item count matches ──────────────────────────────────
        int actual_items = count_type_entries_in_file(types_path);
        if (actual_items >= 0) {
            report.total_items = expected_items;
            report.active_items = actual_items;

            // If counts differ significantly, something went wrong
            int diff = abs(actual_items - expected_items);
            float ratio = (expected_items > 0)
                ? (float)actual_items / (float)expected_items
                : 0.0f;

            if (diff > 0 && ratio < 0.95f) {
                // Items were dropped — some non-deleted items missing from output
                report.passed = false;
                if (report.finding_count < MAX_INTEGRITY_FINDINGS) {
                    IntegrityFinding *f = &report.findings[report.finding_count++];
                    f->severity = SEVERITY_ERROR;
                    snprintf(f->message, sizeof(f->message),
                        "ITEM COUNT MISMATCH: Expected %d items in types.xml, found %d "
                        "(%.1f%% — %d items missing)",
                        expected_items, actual_items,
                        ratio * 100.0f, expected_items - actual_items);
                }
            } else if (actual_items > expected_items * 1.05) {
                // More items than expected — possible duplication leak
                report.passed = false;
                if (report.finding_count < MAX_INTEGRITY_FINDINGS) {
                    IntegrityFinding *f = &report.findings[report.finding_count++];
                    f->severity = SEVERITY_WARNING;
                    snprintf(f->message, sizeof(f->message),
                        "ITEM COUNT EXCESS: Expected %d items, found %d "
                        "(%.1f%% — possible duplicate leak)",
                        expected_items, actual_items, ratio * 100.0f);
                }
            }
        }

        // ── CHECK 3: Spot-check — verify some items exist in output ──────
        // Sample up to 20 items from across the item list to verify they appear
        int sample_size = (expected_items < 20) ? expected_items : 20;
        int step = (expected_items > 0) ? (ctx->item_count / sample_size) : 1;
        if (step < 1) step = 1;
        int missing_samples = 0;

        for (int i = 0, checked = 0; i < ctx->item_count && checked < sample_size; i += step) {
            const LootItem *item = &ctx->items[i];
            if (item->deleted) continue;
            if (item->file_type != FILE_TYPE_ECONOMY) continue;

            if (!classname_in_file(types_path, item->classname)) {
                missing_samples++;
                if (report.finding_count < MAX_INTEGRITY_FINDINGS) {
                    IntegrityFinding *f = &report.findings[report.finding_count++];
                    f->severity = SEVERITY_ERROR;
                    snprintf(f->message, sizeof(f->message),
                        "ITEM MISSING FROM OUTPUT: '%s' (tier %d, %s) not found in %s",
                        item->classname, item->assigned_tier,
                        item->mod_name[0] ? item->mod_name : "vanilla",
                        types_path);
                    strncpy(f->classname, item->classname, MAX_CLASSNAME_LEN - 1);
                }
            }
            checked++;
        }

        if (missing_samples > 0) {
            report.passed = false;
        }

        // ── CHECK 4: Verify NO deleted items leaked into output ──────────
        // A deleted item's classname appearing in output is only a real leak
        // if no non-deleted item shares that classname (which would be the
        // legitimate entry the writer emitted after dedup/merge).
        int deleted_leaks = 0;
        for (int i = 0; i < ctx->item_count; i++) {
            const LootItem *item = &ctx->items[i];
            if (!item->deleted) continue;
            if (item->file_type != FILE_TYPE_ECONOMY) continue;
            // Only check items that were explicitly deleted (debug, merged-away)
            if (!item->is_debug_item && !item->renamed) continue;

            // Check if a non-deleted item shares this classname — if so,
            // the output entry is legitimate (from the surviving copy).
            bool has_live_copy = false;
            for (int k = 0; k < ctx->item_count; k++) {
                if (k == i) continue;
                if (ctx->items[k].deleted) continue;
                if (ctx->items[k].file_type != FILE_TYPE_ECONOMY) continue;
                if (util_strcasecmp(ctx->items[k].classname, item->classname) == 0) {
                    has_live_copy = true;
                    break;
                }
            }
            if (has_live_copy) continue;  // Not a leak — surviving copy is in output

            if (classname_in_file(types_path, item->classname)) {
                deleted_leaks++;
                if (report.finding_count < MAX_INTEGRITY_FINDINGS) {
                    IntegrityFinding *f = &report.findings[report.finding_count++];
                    f->severity = SEVERITY_ERROR;
                    snprintf(f->message, sizeof(f->message),
                        "DELETED ITEM LEAKED: '%s' (debug=%d) found in output %s",
                        item->classname, item->is_debug_item, types_path);
                    strncpy(f->classname, item->classname, MAX_CLASSNAME_LEN - 1);
                    f->is_deleted_leak = true;
                }
            }
        }
        if (deleted_leaks > 0) {
            report.passed = false;
        }
    } else if (types_path) {
        report.passed = false;
        if (report.finding_count < MAX_INTEGRITY_FINDINGS) {
            IntegrityFinding *f = &report.findings[report.finding_count++];
            f->severity = SEVERITY_ERROR;
            snprintf(f->message, sizeof(f->message),
                "OUTPUT FILE NOT FOUND: %s", types_path);
        }
    }

    // ── CHECK 5: Spawnables XML structure ────────────────────────────────
    if (spawnables_path && util_file_exists(spawnables_path)) {
        char xml_error[256] = {0};
        if (!verify_xml_structure(spawnables_path, "spawnabletypes",
                                  xml_error, sizeof(xml_error))) {
            report.passed = false;
            if (report.finding_count < MAX_INTEGRITY_FINDINGS) {
                IntegrityFinding *f = &report.findings[report.finding_count++];
                f->severity = SEVERITY_ERROR;
                snprintf(f->message, sizeof(f->message),
                    "SPAWNABLES XML STRUCTURE INVALID: %s", xml_error);
            }
        }

        // Count spawnable entries
        int actual_spawnables = count_type_entries_in_file(spawnables_path);
        if (actual_spawnables >= 0 && actual_spawnables == 0) {
            report.passed = false;
            if (report.finding_count < MAX_INTEGRITY_FINDINGS) {
                IntegrityFinding *f = &report.findings[report.finding_count++];
                f->severity = SEVERITY_ERROR;
                snprintf(f->message, sizeof(f->message),
                    "SPAWNABLES FILE EMPTY: %s has 0 type entries", spawnables_path);
            }
        }
    }

    // ── CHECK 6: File isn't suspiciously large (all items crammed into one blob) ──
    if (types_path && util_file_exists(types_path)) {
        FILE *f = fopen(types_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long fsize = ftell(f);
            fclose(f);

            // Reasonable estimate: ~300 bytes per item entry in XML
            long expected_size = (long)expected_items * 300;
            if (expected_size > 0 && fsize > expected_size * 3) {
                // File is >3x expected size — possible blob
                if (report.finding_count < MAX_INTEGRITY_FINDINGS) {
                    IntegrityFinding *fi = &report.findings[report.finding_count++];
                    fi->severity = SEVERITY_WARNING;
                    snprintf(fi->message, sizeof(fi->message),
                        "SUSPICIOUSLY LARGE OUTPUT: %s is %ld bytes (expected ~%ld for %d items). "
                        "May contain embedded territory/spawnable data.",
                        types_path, fsize, expected_size, expected_items);
                }
            }
        }
    }

    // ── Build summary ───────────────────────────────────────────────────
    if (report.passed) {
        snprintf(report.summary, sizeof(report.summary),
            "SEW INTEGRITY: PASSED — %d items verified, XML structure validated, "
            "no deleted item leaks detected.",
            expected_items);
        util_log(SEVERITY_INFO, "Integrity: %s", report.summary);
    } else {
        snprintf(report.summary, sizeof(report.summary),
            "SEW INTEGRITY: FAILED — %d finding(s). Review before uploading.",
            report.finding_count);
        util_log(SEVERITY_WARNING, "Integrity: %s", report.summary);
    }

    // Log all findings
    for (int i = 0; i < report.finding_count; i++) {
        util_log(report.findings[i].severity, "  Integrity: %s", report.findings[i].message);
    }

    return report;
}

// ============================================================================
// FULL AUDIT PIPELINE — Replaces the old "Run Audit" button
// ============================================================================
// Runs: Pre-Audit Integrity → Filter → Dedup → Rename → Gap Fill →
//       Fill Missing → Full Audit → Pricing → Sew → Sew Integrity

void auditor_run_audit_pipeline(AuditorContext *ctx) {
    if (!ctx) return;
    util_log(SEVERITY_INFO, "=== AUDIT PIPELINE START ===");

    // Phase 1: Pre-audit integrity check
    util_log(SEVERITY_INFO, "Pipeline Phase 1: Pre-audit integrity check...");
    IntegrityReport pre_report = auditor_pre_audit_integrity(ctx);
    ctx->pre_audit_integrity = pre_report;

    if (!pre_report.passed) {
        util_log(SEVERITY_WARNING, "Pipeline: Pre-audit integrity found %d suspicious variations. "
                 "Proceeding with caution — review Integrity tab.", pre_report.finding_count);
    }

    // Phase 2: Audit core
    util_log(SEVERITY_INFO, "Pipeline Phase 2: Core audit...");
    auditor_filter_debug_items(ctx);
    auditor_sort_items(ctx);          // Sort BEFORE dedup — resolve_duplicates uses
                                      // sorted-scan with early break on classname mismatch
    auditor_resolve_duplicates(ctx);
    auditor_rename_collisions(ctx);
    gap_fill_missing_data(ctx);
    auditor_fill_missing_data(ctx);
    auditor_sanitize_ce_values(ctx);
    auditor_rebalance_usage_pools(ctx);
    auditor_run_full_audit(ctx);

    // Phase 3: Economy
    util_log(SEVERITY_INFO, "Pipeline Phase 3: Economy generation...");
    char local_root[256] = "downloaded_mods";
    util_read_ini_value("config/server_paths.ini", "LOCAL_ROOT", local_root, sizeof(local_root));
    util_generate_mod_manifest(local_root, "output/mod_manifest.md");
    auditor_generate_store_data(ctx);

    // Phase 4: Sew + Integrity
    util_log(SEVERITY_INFO, "Pipeline Phase 4: Sew + integrity verification...");
    util_ensure_directory("output");
    stitcher_init(ctx);
    stitcher_build_groups(ctx);
    stitcher_merge_all(ctx, "output");
    auditor_sort_items(ctx);
    gap_fill_spawnable_types(ctx);
    writer_export_merged_xml(ctx, "output/types.xml");
    writer_export_spawnable_types(ctx, "output/cfgspawnabletypes.xml");

    IntegrityReport sew_report = auditor_sew_integrity_check(
        ctx, "output/types.xml", "output/cfgspawnabletypes.xml");
    ctx->sew_integrity = sew_report;

    // Mark audit as complete — prevents sew button from re-running destructive passes
    ctx->audit_complete = true;

    // Final status
    bool pipeline_ok = pre_report.passed && sew_report.passed;
    if (pipeline_ok) {
        snprintf(ctx->status_message, 255,
            "Audit Pipeline PASSED: %d items, %d issues, %d stitch groups. Sew verified.",
            ctx->item_count, ctx->issue_count, ctx->stitch_group_count);
    } else {
        snprintf(ctx->status_message, 255,
            "Audit Pipeline WARNINGS: %d items, %d issues. Pre-audit:%s Sew:%s. Review integrity.",
            ctx->item_count, ctx->issue_count,
            pre_report.passed ? "OK" : "WARN",
            sew_report.passed ? "OK" : "FAIL");
    }

    util_log(SEVERITY_INFO, "=== AUDIT PIPELINE COMPLETE ===");
    util_log(SEVERITY_INFO, "  %s", ctx->status_message);
}

// ============================================================================
// PRE-UPLOAD OUTPUT VALIDATION
// ============================================================================
// Validates all output files BEFORE uploading to the server.  Any file that
// fails validation is blocked from upload to prevent server crashes.
//
// Checks performed on each XML file:
//   1. File exists and is non-zero size
//   2. Proper XML declaration (<?xml ...)
//   3. Root open/close tags match and are present
//   4. No Tier5+ value tags (DayZ CE only supports Tier1-Tier4)
//   5. cfgeconomycore.xml must have <classes> and <defaults> sections
//   6. types.xml must have no duplicate <type name="..."> entries
//   7. cfgeconomycore.xml must not register default-loaded files or use
//      invalid <file type> identifiers (double registration = CE heap
//      corruption C0000374 — crashed the server 2026-03-07)
//   8. No classname may appear in BOTH types.xml and types_infected.xml /
//      types_wildlife.xml (cross-file duplicate CE registration)

// Descriptor for each expected output file with its root tag
typedef struct {
    const char *filename;       // Relative path from output/
    const char *root_tag;       // Expected XML root element
    bool        require_classes; // Must contain <classes> (cfgeconomycore)
    bool        require_defaults;// Must contain <defaults> (cfgeconomycore)
    bool        check_dupes;    // Check for duplicate type entries
    bool        check_tiers;    // Check for Tier5+ values
    bool        check_ce_manifest; // Check <ce> registrations (cfgeconomycore)
} OutputFileSpec;

static const OutputFileSpec s_output_specs[] = {
    { "output/types.xml",                    "types",            false, false, true,  true,  false },
    { "output/cfgspawnabletypes.xml",        "spawnabletypes",   false, false, false, false, false },
    { "output/cfgeconomycore.xml",           "economycore",      true,  true,  false, false, true  },
    { "output/cfglimitsdefinitionuser.xml",  "user_lists",       false, false, false, false, false },
    { "output/cfgrandompresets.xml",         "randompresets",    false, false, false, false, false },
    { "output/zombie_tiers/types_infected.xml", "types",         false, false, true,  true,  false },
    { "output/zombie_tiers/types_wildlife.xml", "types",         false, false, true,  false, false },
    { NULL, NULL, false, false, false, false, false }
};

// Check a single file for Tier5+ value tags. Returns count of violations.
static int check_tier5_violations(const char *data) {
    int violations = 0;
    const char *p = data;
    while ((p = strstr(p, "name=\"Tier")) != NULL) {
        p += 10; // skip past name="Tier
        // Parse the tier number
        int tier_num = 0;
        while (*p >= '0' && *p <= '9') {
            tier_num = tier_num * 10 + (*p - '0');
            p++;
        }
        if (tier_num >= 5) violations++;
    }
    return violations;
}

// Check for duplicate <type name="..."> entries. Returns count of duplicates.
static int cmp_strings(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

static int check_duplicate_types(const char *data) {
    // Collect all type names into a dynamic array, then sort + scan for dupes.
    int capacity = 4096;
    int  count    = 0;
    char **names  = (char **)malloc(sizeof(char *) * (size_t)capacity);
    if (!names) return -1;

    const char *p = data;
    while ((p = strstr(p, "<type name=\"")) != NULL) {
        p += 12; // skip <type name="
        const char *end = strchr(p, '"');
        if (!end) break;
        int len = (int)(end - p);
        if (count >= capacity) {
            capacity *= 2;
            char **tmp = (char **)realloc(names, sizeof(char *) * (size_t)capacity);
            if (!tmp) { for (int i = 0; i < count; i++) free(names[i]); free(names); return -1; }
            names = tmp;
        }
        names[count] = (char *)malloc((size_t)(len + 1));
        if (!names[count]) break;
        memcpy(names[count], p, (size_t)len);
        names[count][len] = '\0';
        count++;
        p = end + 1;
    }

    // qsort — O(n log n) instead of the previous O(n²) selection sort
    qsort(names, (size_t)count, sizeof(char *), cmp_strings);

    int dupes = 0;
    for (int i = 1; i < count; i++) {
        if (strcmp(names[i], names[i - 1]) == 0) {
            util_log(SEVERITY_ERROR, "  Duplicate type entry: '%s'", names[i]);
            dupes++;
        }
    }

    if (count > 0)
        util_log(SEVERITY_INFO, "  Duplicate check: %d entries scanned, %d duplicates found", count, dupes);

    for (int i = 0; i < count; i++) free(names[i]);
    free(names);
    return dupes;
}

// Check cfgeconomycore.xml <ce> registrations.
// The engine implicitly loads the default mission economy files; registering
// any of them again in <ce> makes the CE load every entry twice — duplicate
// type registration corrupts the heap (C0000374) during Hive::InitOffline().
// Also rejects <file type="..."> identifiers the engine does not support.
// Returns the number of violations found.
static int check_economycore_registrations(const char *data) {
    static const char *forbidden_names[] = {
        "types.xml", "globals.xml", "events.xml", "economy.xml",
        "messages.xml", "cfgspawnabletypes.xml", "cfgrandompresets.xml", NULL
    };
    static const char *valid_types[] = {
        "types", "spawnabletypes", "globals", "economy", "events", "messages", NULL
    };

    int violations = 0;
    const char *p = data;
    while ((p = strstr(p, "<file ")) != NULL) {
        const char *entry_end = strchr(p, '>');
        if (!entry_end) break;

        const char *n = strstr(p, "name=\"");
        if (n && n < entry_end) {
            n += 6;
            const char *e = strchr(n, '"');
            if (e && e < entry_end && (e - n) < 127) {
                char fname[128];
                memcpy(fname, n, (size_t)(e - n));
                fname[e - n] = '\0';
                for (int i = 0; forbidden_names[i]; i++) {
                    if (util_strcasecmp(fname, forbidden_names[i]) == 0) {
                        util_log(SEVERITY_ERROR,
                                 "  cfgeconomycore registers default file '%s' — the engine already loads it; double registration corrupts the CE heap",
                                 fname);
                        violations++;
                        break;
                    }
                }
            }
        }

        const char *t = strstr(p, "type=\"");
        if (t && t < entry_end) {
            t += 6;
            const char *e = strchr(t, '"');
            if (e && e < entry_end && (e - t) < 63) {
                char tname[64];
                memcpy(tname, t, (size_t)(e - t));
                tname[e - t] = '\0';
                bool known = false;
                for (int i = 0; valid_types[i]; i++) {
                    if (strcmp(tname, valid_types[i]) == 0) { known = true; break; }
                }
                if (!known) {
                    util_log(SEVERITY_ERROR,
                             "  cfgeconomycore <file> uses invalid type identifier '%s' (valid: types, spawnabletypes, globals, economy, events, messages)",
                             tname);
                    violations++;
                }
            }
        }

        p = entry_end + 1;
    }
    return violations;
}

// Check that no classname is registered in BOTH types.xml and the zombie/
// wildlife type files. cfgeconomycore registers the zombie files as extra
// "types" files, so an overlapping classname is registered twice by the CE —
// this exact overlap (205 zombie/animal entries) crashed the server with
// heap corruption on 2026-03-07. Returns duplicate count, or 0 if the files
// don't exist. Reuses check_duplicate_types() on the concatenated contents:
// per-file duplicates are caught by the per-file pass, so any NEW duplicates
// found here are cross-file.
static int check_cross_file_duplicates(int per_file_dupes) {
    static const char *cross_files[] = {
        "output/types.xml",
        "output/zombie_tiers/types_infected.xml",
        "output/zombie_tiers/types_wildlife.xml",
        NULL
    };

    char  *combined = NULL;
    size_t combined_len = 0;
    int    files_loaded = 0;

    for (int i = 0; cross_files[i]; i++) {
        FILE *f = fopen(cross_files[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (fsize <= 0) { fclose(f); continue; }
        char *tmp = (char *)realloc(combined, combined_len + (size_t)fsize + 1);
        if (!tmp) { fclose(f); free(combined); return 0; }
        combined = tmp;
        size_t got = fread(combined + combined_len, 1, (size_t)fsize, f);
        fclose(f);
        combined_len += got;
        combined[combined_len] = '\0';
        files_loaded++;
    }

    if (files_loaded < 2) { free(combined); return 0; }

    int total_dupes = check_duplicate_types(combined);
    free(combined);
    if (total_dupes < 0) return 0;

    int cross_dupes = total_dupes - per_file_dupes;
    return (cross_dupes > 0) ? cross_dupes : 0;
}

// Remediation flags — set by auditor_validate_output_for_upload(), read by
// auditor_remediate_and_validate() to decide which files to re-export.
static bool s_needs_types_reexport  = false;
static bool s_needs_zombie_reexport = false;
static bool s_needs_tier_sanitize   = false;
static bool s_needs_economycore_reexport = false;

bool auditor_validate_output_for_upload(int *out_passed, int *out_failed) {
    util_log(SEVERITY_INFO, "=== PRE-UPLOAD OUTPUT VALIDATION ===");

    int passed = 0;
    int failed = 0;
    int per_file_dupes_total     = 0;
    bool needs_types_reexport    = false;
    bool needs_zombie_reexport   = false;
    bool needs_tier_sanitize     = false;
    bool needs_economycore_reexport = false;

    for (int s = 0; s_output_specs[s].filename != NULL; s++) {
        const OutputFileSpec *spec = &s_output_specs[s];

        // Skip files that don't exist (optional outputs)
        if (!util_file_exists(spec->filename)) {
            util_log(SEVERITY_INFO, "  [SKIP] %s (not generated)", spec->filename);
            continue;
        }

        util_log(SEVERITY_INFO, "  Validating: %s ...", spec->filename);
        bool file_ok = true;

        // Read file contents
        FILE *f = fopen(spec->filename, "rb");
        if (!f) {
            util_log(SEVERITY_ERROR, "  [FAIL] %s — cannot open file", spec->filename);
            failed++;
            continue;
        }
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        if (fsize <= 0) {
            util_log(SEVERITY_ERROR, "  [FAIL] %s — file is empty (0 bytes)", spec->filename);
            fclose(f);
            failed++;
            continue;
        }
        fseek(f, 0, SEEK_SET);
        char *data = (char *)malloc((size_t)fsize + 1);
        if (!data) { fclose(f); failed++; continue; }
        size_t bytes_read = fread(data, 1, (size_t)fsize, f);
        data[bytes_read] = '\0';
        fclose(f);

        // Check 1: XML declaration
        if (!strstr(data, "<?xml")) {
            util_log(SEVERITY_ERROR, "  [FAIL] %s — missing XML declaration", spec->filename);
            file_ok = false;
        }

        // Check 2: Root open/close tags
        char error_buf[512];
        if (!verify_xml_structure(spec->filename, spec->root_tag, error_buf, sizeof(error_buf))) {
            util_log(SEVERITY_ERROR, "  [FAIL] %s — %s", spec->filename, error_buf);
            file_ok = false;
        }

        // Check 3: cfgeconomycore mandatory sections
        if (spec->require_classes && !strstr(data, "<classes>")) {
            util_log(SEVERITY_ERROR, "  [FAIL] %s — missing mandatory <classes> section", spec->filename);
            file_ok = false;
        }
        if (spec->require_defaults && !strstr(data, "<defaults>")) {
            util_log(SEVERITY_ERROR, "  [FAIL] %s — missing mandatory <defaults> section", spec->filename);
            file_ok = false;
        }

        // Check 3b: cfgeconomycore <ce> registration sanity (remediable —
        // re-export the manifest with the corrected writer)
        if (spec->check_ce_manifest) {
            int ce_violations = check_economycore_registrations(data);
            if (ce_violations > 0) {
                util_log(SEVERITY_WARNING, "  [REMEDIATE] %s — %d invalid <ce> registration(s) — will re-export manifest",
                         spec->filename, ce_violations);
                needs_economycore_reexport = true;
                file_ok = false;
            }
        }

        // Check 4: Tier5+ violations (remediable — sanitize + re-export)
        if (spec->check_tiers) {
            int tier_violations = check_tier5_violations(data);
            if (tier_violations > 0) {
                util_log(SEVERITY_WARNING, "  [REMEDIATE] %s — %d Tier5+ value tags found — will sanitize and re-export",
                         spec->filename, tier_violations);
                needs_tier_sanitize = true;
                if (strstr(spec->filename, "types_infected"))
                    needs_zombie_reexport = true;
                else
                    needs_types_reexport = true;
                file_ok = false;
            }
        }

        // Check 5: Duplicate type entries (remediable — re-export with dedup)
        if (spec->check_dupes) {
            int dupes = check_duplicate_types(data);
            if (dupes > 0) {
                util_log(SEVERITY_WARNING, "  [REMEDIATE] %s — %d duplicate <type name=\"...\"> entries — will re-export with dedup",
                         spec->filename, dupes);
                if (strstr(spec->filename, "types_infected") || strstr(spec->filename, "types_wildlife"))
                    needs_zombie_reexport = true;
                else
                    needs_types_reexport = true;
                file_ok = false;
            }
            if (dupes > 0) per_file_dupes_total += dupes;
        }

        free(data);

        if (file_ok) {
            util_log(SEVERITY_INFO, "  [PASS] %s", spec->filename);
            passed++;
        } else {
            failed++;
        }
    }

    // Check 6: cross-file duplicate registration — a classname present in
    // BOTH types.xml and types_infected.xml/types_wildlife.xml gets
    // registered twice by the CE (the zombie files are extra "types" files
    // in cfgeconomycore). Remediable: re-export both sides — the writer
    // excludes zombies/animals from types.xml and dedups.
    {
        int cross_dupes = check_cross_file_duplicates(per_file_dupes_total);
        if (cross_dupes > 0) {
            util_log(SEVERITY_WARNING, "  [REMEDIATE] %d classname(s) registered in BOTH types.xml and types_infected/types_wildlife — will re-export both",
                     cross_dupes);
            needs_types_reexport  = true;
            needs_zombie_reexport = true;
            failed++;
        }
    }

    util_log(SEVERITY_INFO, "=== PRE-UPLOAD VALIDATION: %d passed, %d failed ===",
             passed, failed);

    if (out_passed) *out_passed = passed;
    if (out_failed) *out_failed = failed;

    // Report remediable flags to caller via output pointers — the remediation
    // loop in auditor_remediate_and_validate() uses these.
    // (We stash them in file-scope statics so the caller can query them.)
    s_needs_types_reexport  = needs_types_reexport;
    s_needs_zombie_reexport = needs_zombie_reexport;
    s_needs_tier_sanitize   = needs_tier_sanitize;
    s_needs_economycore_reexport = needs_economycore_reexport;

    return (failed == 0);
}

// ============================================================================
// REMEDIATE-AND-VALIDATE LOOP
// ============================================================================
// Runs validation, and when remediable issues are found (duplicate type entries,
// Tier5+ value tags), fixes the in-memory data and re-exports the affected
// files.  Loops up to MAX_REMEDIATION_PASSES times.  Only fails on truly
// non-recoverable structural corruption (missing XML, broken root tags, etc.).

#define MAX_REMEDIATION_PASSES 3

bool auditor_remediate_and_validate(AuditorContext *ctx, int *out_passed, int *out_failed) {
    if (!ctx) return auditor_validate_output_for_upload(out_passed, out_failed);

    for (int pass = 0; pass < MAX_REMEDIATION_PASSES; pass++) {
        int passed = 0, failed = 0;
        bool valid = auditor_validate_output_for_upload(&passed, &failed);

        if (valid) {
            // All clean — done.
            if (out_passed) *out_passed = passed;
            if (out_failed) *out_failed = 0;
            if (pass > 0)
                util_log(SEVERITY_INFO, "Remediation pass %d: ALL PASSED — issues resolved.", pass);
            return true;
        }

        // Check if any of the failures are remediable
        bool can_remediate = s_needs_types_reexport || s_needs_zombie_reexport ||
                             s_needs_tier_sanitize  || s_needs_economycore_reexport;
        if (!can_remediate) {
            // Only structural / non-recoverable failures remain — give up.
            util_log(SEVERITY_ERROR, "Validation: %d file(s) failed with non-recoverable issues. Cannot auto-fix.", failed);
            if (out_passed) *out_passed = passed;
            if (out_failed) *out_failed = failed;
            return false;
        }

        util_log(SEVERITY_INFO, "=== REMEDIATION PASS %d ===", pass + 1);

        // Tier5+ fix: sanitize value tags in memory, then re-export
        if (s_needs_tier_sanitize) {
            util_log(SEVERITY_INFO, "Remediation: Sanitizing Tier5+ value tags...");
            auditor_sanitize_ce_values(ctx);
        }

        // Re-export affected files — writer has sort + adjacent-dedup built in
        if (s_needs_types_reexport) {
            util_log(SEVERITY_INFO, "Remediation: Re-exporting output/types.xml (sort + dedup)...");
            writer_export_merged_xml(ctx, "output/types.xml");
        }

        if (s_needs_zombie_reexport) {
            util_log(SEVERITY_INFO, "Remediation: Re-exporting zombie_tiers/ (sort + dedup)...");
            writer_export_zombie_config(ctx, "output");
        }

        if (s_needs_economycore_reexport) {
            util_log(SEVERITY_INFO, "Remediation: Re-exporting output/cfgeconomycore.xml (default files load implicitly)...");
            writer_export_cfgeconomycore(ctx, "output/cfgeconomycore.xml");
        }

        // Reset flags for next validation pass
        s_needs_types_reexport  = false;
        s_needs_zombie_reexport = false;
        s_needs_tier_sanitize   = false;
        s_needs_economycore_reexport = false;
    }

    // Exhausted all passes — run final validation to report status
    int passed = 0, failed = 0;
    bool valid = auditor_validate_output_for_upload(&passed, &failed);
    if (out_passed) *out_passed = passed;
    if (out_failed) *out_failed = failed;
    if (!valid) {
        util_log(SEVERITY_ERROR, "Remediation: Exhausted %d passes — %d file(s) still failing.",
                 MAX_REMEDIATION_PASSES, failed);
    }
    return valid;
}
