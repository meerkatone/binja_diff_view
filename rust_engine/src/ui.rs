use crate::{DiffResult, FunctionMatch, MatchType};
use std::collections::HashMap;

pub struct DiffUI;

struct ReportColors {
    header: &'static str,
    label: &'static str,
    good: &'static str,
    bad: &'static str,
    info: &'static str,
    separator: &'static str,
    bold: &'static str,
    reset: &'static str,
    match_exact: &'static str,
    match_structural: &'static str,
    match_heuristic: &'static str,
    match_manual: &'static str,
    confidence_high: &'static str,
    confidence_medium: &'static str,
    confidence_low: &'static str,
}

impl ReportColors {
    fn plain() -> Self {
        Self {
            header: "",
            label: "",
            good: "",
            bad: "",
            info: "",
            separator: "",
            bold: "",
            reset: "",
            match_exact: "",
            match_structural: "",
            match_heuristic: "",
            match_manual: "",
            confidence_high: "",
            confidence_medium: "",
            confidence_low: "",
        }
    }

    fn ansi() -> Self {
        Self {
            header: "\x1b[36m",
            label: "\x1b[34m",
            good: "\x1b[32m",
            bad: "\x1b[31m",
            info: "\x1b[36m",
            separator: "\x1b[33m",
            bold: "\x1b[1m",
            reset: "\x1b[0m",
            match_exact: "\x1b[32m",
            match_structural: "\x1b[33m",
            match_heuristic: "\x1b[35m",
            match_manual: "\x1b[36m",
            confidence_high: "\x1b[32m",
            confidence_medium: "\x1b[33m",
            confidence_low: "\x1b[31m",
        }
    }
}

fn generate_report_with_colors(diff_result: &DiffResult, c: &ReportColors) -> String {
    let mut report = String::new();

    // Header
    report.push_str(&format!(
        "{}{}{}\n{}{}                 BINARY DIFF REPORT{}\n{}{}{}\n\n",
        c.bold,
        c.header,
        "=".repeat(60),
        c.bold,
        c.header,
        c.reset,
        c.bold,
        c.header,
        "=".repeat(60),
    ));

    // Summary
    report.push_str(&format!("{}{}SUMMARY:{}\n", c.bold, c.label, c.reset));
    report.push_str(&format!(
        "  {}Total Matches:{} {}\n",
        c.good,
        c.reset,
        diff_result.matched_functions.len()
    ));
    report.push_str(&format!(
        "  {}Unmatched Functions A:{} {}\n",
        c.bad,
        c.reset,
        diff_result.unmatched_functions_a.len()
    ));
    report.push_str(&format!(
        "  {}Unmatched Functions B:{} {}\n",
        c.bad,
        c.reset,
        diff_result.unmatched_functions_b.len()
    ));
    report.push_str(&format!(
        "  {}Overall Similarity:{} {:.4}\n\n",
        c.info, c.reset, diff_result.similarity_score
    ));

    // Match type breakdown
    let match_counts = count_match_types(&diff_result.matched_functions);
    report.push_str(&format!(
        "{}{}MATCH TYPE BREAKDOWN:{}\n",
        c.bold, c.label, c.reset
    ));
    // One line per matching phase, so the counts add up to Total Matches.
    // Listing only four of the eight types silently dropped name, MD-index,
    // small-primes and call-graph matches from the breakdown.
    for (label, match_type) in [
        ("Exact", MatchType::Exact),
        ("Name", MatchType::Name),
        ("MD-Index", MatchType::MdIndex),
        ("Small-Primes", MatchType::SmallPrimes),
        ("Structural", MatchType::Structural),
        ("Call-Graph", MatchType::CallGraph),
        ("Heuristic", MatchType::Heuristic),
        ("Manual", MatchType::Manual),
    ] {
        report.push_str(&format!(
            "  {} Matches: {}\n",
            label,
            match_counts.get(&match_type).unwrap_or(&0)
        ));
    }
    report.push('\n');

    // Detailed matches
    report.push_str(&format!(
        "{}{}DETAILED MATCHES:{}\n",
        c.bold, c.label, c.reset
    ));
    report.push_str(&format!("{}{}{}\n", c.separator, "-".repeat(60), c.reset));

    let mut sorted_matches = diff_result.matched_functions.clone();
    sorted_matches.sort_by(|a, b| {
        b.confidence
            .total_cmp(&a.confidence)
            .then_with(|| a.function_a.address.cmp(&b.function_a.address))
            .then_with(|| a.function_b.address.cmp(&b.function_b.address))
    });

    for (i, m) in sorted_matches.iter().enumerate() {
        let cc = if m.confidence > 0.8 {
            c.confidence_high
        } else if m.confidence > 0.6 {
            c.confidence_medium
        } else {
            c.confidence_low
        };

        let mc = match m.match_type {
            MatchType::Exact | MatchType::Name => c.match_exact,
            MatchType::MdIndex | MatchType::SmallPrimes | MatchType::Structural => {
                c.match_structural
            }
            MatchType::CallGraph | MatchType::Heuristic => c.match_heuristic,
            MatchType::Manual => c.match_manual,
        };

        report.push_str(&format!(
            "{}{}. {}{} <-> {}{}\n",
            c.bold,
            i + 1,
            c.good,
            m.function_a.name,
            m.function_b.name,
            c.reset
        ));
        report.push_str(&format!(
            "   Addresses: {}0x{:x}{} <-> {}0x{:x}{}\n",
            c.info, m.function_a.address, c.reset, c.info, m.function_b.address, c.reset
        ));
        report.push_str(&format!(
            "   Similarity: {}{:.4}{} | Confidence: {}{:.4}{} | Type: {}{:?}{}\n",
            cc, m.similarity, c.reset, cc, m.confidence, c.reset, mc, m.match_type, c.reset
        ));
        report.push_str(&format!(
            "   Size: {} bytes <-> {} bytes\n",
            m.function_a.size, m.function_b.size
        ));
        report.push_str(&format!(
            "   Basic Blocks: {} <-> {}\n",
            m.function_a.basic_blocks.len(),
            m.function_b.basic_blocks.len()
        ));
        // Result functions are slimmed: their instruction lists are empty
        // and the count lives in `instruction_count`.
        report.push_str(&format!(
            "   Instructions: {} <-> {}\n\n",
            m.function_a.instruction_count, m.function_b.instruction_count
        ));
    }

    // Unmatched functions
    for (label, funcs) in [
        ("BINARY A", &diff_result.unmatched_functions_a),
        ("BINARY B", &diff_result.unmatched_functions_b),
    ] {
        if !funcs.is_empty() {
            report.push_str(&format!("UNMATCHED FUNCTIONS IN {}:\n", label));
            report.push_str(&format!("{}{}{}\n", c.separator, "-".repeat(60), c.reset));
            for func in funcs {
                report.push_str(&format!(
                    "  {} (0x{:x}) - {} bytes, {} BBs\n",
                    func.name,
                    func.address,
                    func.size,
                    func.basic_blocks.len()
                ));
            }
            report.push('\n');
        }
    }

    report
}

fn count_match_types(matches: &[FunctionMatch]) -> HashMap<MatchType, usize> {
    let mut counts = HashMap::new();
    for m in matches {
        *counts.entry(m.match_type.clone()).or_insert(0) += 1;
    }
    counts
}

/// Shorten a name to the table's 28-character column. Counts characters, not
/// bytes: slicing a demangled or non-ASCII name at byte 25 panicked when that
/// byte was inside a multi-byte character.
fn table_name(name: &str) -> String {
    if name.chars().count() > 28 {
        let head: String = name.chars().take(25).collect();
        format!("{head}...")
    } else {
        name.to_string()
    }
}

impl DiffUI {
    /// Generate a plain text diff report
    pub fn generate_text_report(diff_result: &DiffResult) -> String {
        generate_report_with_colors(diff_result, &ReportColors::plain())
    }

    /// Generate a color-coded terminal output
    pub fn generate_colored_report(diff_result: &DiffResult) -> String {
        generate_report_with_colors(diff_result, &ReportColors::ansi())
    }

    /// Generate a progress bar for diff operations
    pub fn generate_progress_bar(current: usize, total: usize, width: usize) -> String {
        if total == 0 {
            return String::new();
        }

        // `current` can overshoot `total` when work is discovered late; an
        // unclamped bar underflowed `width - filled` and panicked.
        let fraction = (current as f64 / total as f64).clamp(0.0, 1.0);
        let filled_cells = ((fraction * width as f64) as usize).min(width);
        let filled = "█".repeat(filled_cells);
        let empty = "░".repeat(width - filled_cells);

        format!(
            "[{}{}] {}/{} ({:.1}%)",
            filled,
            empty,
            current,
            total,
            (current as f64 / total as f64) * 100.0
        )
    }

    /// Generate a simple diff visualization
    pub fn generate_diff_visualization(match_result: &FunctionMatch) -> String {
        let mut viz = String::new();

        viz.push_str(&format!(
            "Function Comparison: {} vs {}\n",
            match_result.function_a.name, match_result.function_b.name
        ));
        viz.push_str("=".repeat(50).as_str());
        viz.push('\n');

        viz.push_str("Basic Block Comparison:\n");
        viz.push_str(&format!(
            "  Function A: {} blocks\n",
            match_result.function_a.basic_blocks.len()
        ));
        viz.push_str(&format!(
            "  Function B: {} blocks\n",
            match_result.function_b.basic_blocks.len()
        ));

        let max_blocks = match_result
            .function_a
            .basic_blocks
            .len()
            .max(match_result.function_b.basic_blocks.len());

        for i in 0..max_blocks {
            let block_a = match_result.function_a.basic_blocks.get(i);
            let block_b = match_result.function_b.basic_blocks.get(i);

            match (block_a, block_b) {
                (Some(a), Some(b)) => {
                    viz.push_str(&format!(
                        "  Block {}: 0x{:x} ({} instrs) | 0x{:x} ({} instrs)\n",
                        i, a.address, a.instruction_count, b.address, b.instruction_count
                    ));
                }
                (Some(a), None) => {
                    viz.push_str(&format!(
                        "  Block {}: 0x{:x} ({} instrs) | <missing>\n",
                        i, a.address, a.instruction_count
                    ));
                }
                (None, Some(b)) => {
                    viz.push_str(&format!(
                        "  Block {}: <missing> | 0x{:x} ({} instrs)\n",
                        i, b.address, b.instruction_count
                    ));
                }
                (None, None) => break,
            }
        }

        viz.push_str("\nInstruction Statistics:\n");
        viz.push_str(&format!(
            "  Function A: {} instructions\n",
            match_result.function_a.instruction_count
        ));
        viz.push_str(&format!(
            "  Function B: {} instructions\n",
            match_result.function_b.instruction_count
        ));
        viz.push_str(&format!(
            "  Complexity A: {}\n",
            match_result.function_a.cyclomatic_complexity
        ));
        viz.push_str(&format!(
            "  Complexity B: {}\n",
            match_result.function_b.cyclomatic_complexity
        ));

        viz
    }

    /// Generate a summary table for matches
    pub fn generate_summary_table(matches: &[FunctionMatch]) -> String {
        let mut table = String::new();

        table.push_str("┌─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐\n");
        table.push_str("│                                                    FUNCTION MATCHES                                                    │\n");
        table.push_str("├─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤\n");
        table.push_str("│ Function A                    │ Function B                    │ Similarity │ Confidence │ Type       │ Size A │ Size B │ BB A │ BB B │\n");
        table.push_str("├─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤\n");

        for m in matches {
            let func_a_name = table_name(&m.function_a.name);
            let func_b_name = table_name(&m.function_b.name);

            table.push_str(&format!(
                "│ {:<29} │ {:<29} │ {:<10.4} │ {:<10.4} │ {:<10?} │ {:<6} │ {:<6} │ {:<4} │ {:<4} │\n",
                func_a_name,
                func_b_name,
                m.similarity,
                m.confidence,
                m.match_type,
                m.function_a.size,
                m.function_b.size,
                m.function_a.basic_blocks.len(),
                m.function_b.basic_blocks.len()
            ));
        }

        table.push_str("└─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘\n");

        table
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{BasicBlockInfo, BinaryDiffEngine, FunctionInfo, InstructionInfo};

    fn function(name: &str, address: u64) -> FunctionInfo {
        let instructions: Vec<InstructionInfo> = ["push", "mov", "add", "pop", "ret"]
            .into_iter()
            .enumerate()
            .map(|(index, mnemonic)| InstructionInfo {
                address: address + index as u64,
                mnemonic: mnemonic.into(),
                length: 1,
                ..Default::default()
            })
            .collect();
        FunctionInfo {
            name: name.into(),
            address,
            size: 5,
            basic_blocks: vec![BasicBlockInfo {
                address,
                size: 5,
                instructions,
                ..Default::default()
            }],
            ..Default::default()
        }
    }

    fn diff_result() -> DiffResult {
        let a = vec![function("parse_header", 0x1000)];
        let b = vec![function("parse_header", 0x2000)];
        BinaryDiffEngine::new()
            .perform_diff_json(
                &serde_json::to_string(&a).unwrap(),
                &serde_json::to_string(&b).unwrap(),
            )
            .unwrap()
    }

    #[test]
    fn reports_use_the_counts_kept_on_slimmed_result_functions() {
        let result = diff_result();
        let [matched] = result.matched_functions.as_slice() else {
            panic!("expected one match");
        };
        // The engine drops instruction payloads from results.
        assert!(matched.function_a.instructions.is_empty());
        let report = DiffUI::generate_text_report(&result);
        assert!(report.contains("Instructions: 5 <-> 5"), "{report}");
        let visualization = DiffUI::generate_diff_visualization(matched);
        assert!(visualization.contains("(5 instrs)"), "{visualization}");
        assert!(visualization.contains("Function A: 5 instructions"));
    }

    #[test]
    fn match_type_breakdown_accounts_for_every_match() {
        let result = diff_result();
        let report = DiffUI::generate_text_report(&result);
        let listed: usize = report
            .lines()
            .filter_map(|line| line.trim().split_once(" Matches: "))
            .filter(|(label, _)| *label != "Total")
            .map(|(_, count)| count.parse::<usize>().unwrap())
            .sum();
        assert_eq!(listed, result.matched_functions.len());
        assert!(report.contains("Name Matches: ") && report.contains("Call-Graph Matches: "));
    }

    #[test]
    fn summary_table_truncates_non_ascii_names_without_panicking() {
        let mut matched = FunctionMatch::default();
        // 30 two-byte characters: byte 25 falls inside a character.
        matched.function_a.name = "é".repeat(30);
        matched.function_b.name = "short".into();
        let table = DiffUI::generate_summary_table(&[matched]);
        assert!(table.contains(&format!("{}...", "é".repeat(25))));
        assert!(table.contains("short"));
        assert_eq!(table_name(&"x".repeat(28)), "x".repeat(28));
    }

    #[test]
    fn progress_bar_is_clamped_to_its_width() {
        assert_eq!(DiffUI::generate_progress_bar(0, 0, 10), "");
        let half = DiffUI::generate_progress_bar(5, 10, 10);
        assert_eq!(half.matches('█').count(), 5);
        assert_eq!(half.matches('░').count(), 5);
        let over = DiffUI::generate_progress_bar(15, 10, 10);
        assert_eq!(over.matches('█').count(), 10);
        assert_eq!(over.matches('░').count(), 0);
        assert!(over.contains("15/10"));
    }
}
