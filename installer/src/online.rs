//! Online play over Epic comes as a separate "online pack" (the Epic-enabled
//! runtime, co-op DLL, Epic DLL and eos.ini) from the GitHub release
//! "online-pack". It is installed only when it was built from exactly the
//! source that was just built; the stamp is the same one
//! scripts/online_stamp.ps1 computes.

use crate::config;
use crate::fsx;
use crate::report::{self, Reporter};
use anyhow::{Context, Result, bail};
use sha2::{Digest, Sha256};
use std::cmp::Ordering;
use std::fs::{self, File};
use std::io::{self, Read};
use std::path::Path;

/// PowerShell's Sort-Object order for the lower-case paths used here:
/// '-' is ignored, then '.' < '\' < '_' < other symbols < digits < letters.
fn ps_order(a: &str, b: &str) -> Ordering {
    fn weights(s: &str) -> Vec<u32> {
        s.chars()
            .filter(|&c| c != '-')
            .map(|c| match c {
                '.' => 1,
                '\\' => 2,
                '_' => 3,
                '0'..='9' => 100 + c as u32,
                'a'..='z' => 1000 + c as u32,
                _ => 10 + (c as u32 % 80),
            })
            .collect()
    }
    weights(a).cmp(&weights(b)).then_with(|| a.cmp(b))
}

pub fn stamp(root: &Path, sdk_commit: &str) -> Result<String> {
    let mut files: Vec<(String, std::path::PathBuf)> = Vec::new();
    files.push((
        "\\patches\\rexglue-sdk.patch".into(),
        root.join("patches").join("rexglue-sdk.patch"),
    ));
    for (dir, rel, exts) in [
        (
            root.join("core").join("WhompaysCoop"),
            "\\core\\whompayscoop\\",
            &["cpp", "h"][..],
        ),
        (
            root.join("modding").join("include"),
            "\\modding\\include\\",
            &["h"][..],
        ),
    ] {
        for e in fs::read_dir(&dir).with_context(|| format!("could not read {}", dir.display()))? {
            let e = e?;
            if !e.file_type()?.is_file() {
                continue;
            }
            let p = e.path();
            let ext = p
                .extension()
                .map(|x| x.to_string_lossy().to_ascii_lowercase())
                .unwrap_or_default();
            if exts.contains(&ext.as_str()) {
                let name = e.file_name().to_string_lossy().to_lowercase();
                files.push((format!("{rel}{name}"), p));
            }
        }
    }
    files.sort_by(|a, b| ps_order(&a.0, &b.0));
    let mut text: Vec<u8> = format!("sdk {sdk_commit}\n").into_bytes();
    for (key, path) in &files {
        let rel = key.trim_start_matches('\\').replace('\\', "/");
        text.extend_from_slice(format!("== {rel} ==\n").as_bytes());
        let bytes = fs::read(path).with_context(|| format!("could not read {}", path.display()))?;
        text.extend(bytes.into_iter().filter(|&b| b != b'\r'));
    }
    Ok(report::hex(&Sha256::digest(&text)))
}

/// Ok(true) installed, Ok(false) the pack is for another version.
pub fn install(
    r: &Reporter,
    root: &Path,
    dist: &Path,
    sdk_commit: &str,
    downloads: &Path,
) -> Result<bool> {
    let wanted = stamp(root, sdk_commit)?;
    fsx::write_text(&dist.join("online_pack_wanted.txt"), &wanted)?;
    let installed = dist.join("online_pack.txt");
    let eos_dll = dist
        .join("core")
        .join("WhompaysCoop")
        .join("eos")
        .join("EOSSDK-Win64-Shipping.dll");
    if fsx::read_text(&installed).as_deref() == Some(wanted.as_str()) && eos_dll.exists() {
        r.log("Online play: on (online pack already installed).");
        return Ok(true);
    }
    let zip_path = downloads.join("SaintsReborn-Online.zip");
    let _ = fs::remove_file(&zip_path);
    report::download(r, config::ONLINE_PACK_URL, &zip_path, None)?;
    let mut zip = zip::ZipArchive::new(File::open(&zip_path)?)?;
    let have = {
        let mut e = zip
            .by_name("stamp.txt")
            .context("the online pack has no stamp.txt")?;
        let mut s = String::new();
        e.read_to_string(&mut s)?;
        s.trim().to_string()
    };
    if have != wanted {
        r.log(
            "Online play: the online pack is for another version of the source, so online play stays off \
             for now (System Link on a LAN and co-op by IP still work).",
        );
        let _ = fs::remove_file(&zip_path);
        return Ok(false);
    }
    for i in 0..zip.len() {
        let mut e = zip.by_index(i)?;
        let name = e.name().replace('\\', "/");
        if e.is_dir() || name == "stamp.txt" {
            continue;
        }
        let Some(rel) = fsx::safe_relative(&name) else {
            bail!("bad path in the online pack: {name}")
        };
        let dest = dist.join(rel);
        if let Some(p) = dest.parent() {
            fs::create_dir_all(p)?;
        }
        let mut f = File::create(&dest)?;
        io::copy(&mut e, &mut f)?;
    }
    if !eos_dll.exists() {
        bail!("the Epic DLL is missing after installing the online pack");
    }
    fsx::write_text(&installed, &wanted)?;
    let _ = fs::remove_file(&zip_path);
    r.log("Online play: on (online pack installed).");
    Ok(true)
}
