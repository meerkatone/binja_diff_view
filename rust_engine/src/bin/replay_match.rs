use anyhow::{bail, Context, Result};
use rust_diff::BinaryDiffEngine;
use sha2::{Digest, Sha256};
use std::env;
use std::fs;
use std::path::Path;

fn enrich_strings(payload: &str, path: Option<&Path>) -> Result<String> {
    let Some(path) = path else {
        return Ok(payload.to_string());
    };
    let enrichment: std::collections::HashMap<String, Vec<String>> = serde_json::from_slice(
        &fs::read(path)
            .with_context(|| format!("failed to read string enrichment {}", path.display()))?,
    )
    .with_context(|| format!("failed to parse string enrichment {}", path.display()))?;
    let mut functions: Vec<rust_diff::FunctionInfo> = serde_json::from_str(payload)?;
    for function in &mut functions {
        let decimal = function.address.to_string();
        let hexadecimal = format!("0x{:x}", function.address);
        if let Some(strings) = enrichment
            .get(&decimal)
            .or_else(|| enrichment.get(&hexadecimal))
        {
            function.strings.extend(strings.iter().cloned());
        }
    }
    Ok(serde_json::to_string(&functions)?)
}

fn extraction_payload(path: &Path) -> Result<String> {
    let contents = fs::read(path)
        .with_context(|| format!("failed to read extraction cache {}", path.display()))?;
    let mut newlines = contents
        .iter()
        .enumerate()
        .filter_map(|(index, byte)| (*byte == b'\n').then_some(index));
    let first = newlines.next().context("missing extraction cache magic")?;
    let second = newlines
        .next()
        .context("missing extraction cache identity")?;
    let third = newlines
        .next()
        .context("missing extraction cache checksum")?;
    if &contents[..first] != b"binja-diff-extraction-cache-v1" || second <= first || third <= second
    {
        bail!("invalid extraction cache header in {}", path.display());
    }
    let payload = &contents[third + 1..];
    let expected = &contents[second + 1..third];
    let actual = hex::encode(Sha256::digest(payload));
    if expected != actual.as_bytes() {
        bail!("extraction cache checksum mismatch in {}", path.display());
    }
    String::from_utf8(payload.to_vec())
        .with_context(|| format!("extraction payload is not UTF-8 in {}", path.display()))
}

fn main() -> Result<()> {
    let mut args = env::args_os().skip(1);
    let path_a = args
        .next()
        .context("usage: replay_match CACHE_A CACHE_B [OUTPUT_JSON] [STRINGS_B_JSON]")?;
    let path_b = args
        .next()
        .context("usage: replay_match CACHE_A CACHE_B [OUTPUT_JSON] [STRINGS_B_JSON]")?;
    let output = args.next();
    let strings_b = args.next();
    if args.next().is_some() {
        bail!("usage: replay_match CACHE_A CACHE_B [OUTPUT_JSON] [STRINGS_B_JSON]");
    }

    let json_a = extraction_payload(Path::new(&path_a))?;
    let json_b = enrich_strings(
        &extraction_payload(Path::new(&path_b))?,
        strings_b.as_deref().map(Path::new),
    )?;
    let result = BinaryDiffEngine::new().perform_diff_json(&json_a, &json_b)?;
    if let Some(output) = output {
        fs::write(&output, serde_json::to_vec_pretty(&result)?)
            .with_context(|| format!("failed to write {}", Path::new(&output).display()))?;
    }
    println!(
        "matches={} unmatched_a={} unmatched_b={} similarity={:.9} matched_similarity={:.9} coverage={:.9} time={:.3}s",
        result.matched_functions.len(),
        result.unmatched_functions_a.len(),
        result.unmatched_functions_b.len(),
        result.similarity_score,
        result.matched_similarity_score,
        result.match_coverage,
        result.analysis_time
    );
    Ok(())
}
