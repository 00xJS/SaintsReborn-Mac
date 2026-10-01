//! File helpers: unpacking archives and copying folders.

use crate::report::{Env, Reporter, run};
use anyhow::{Context, Result, bail};
use std::fs::{self, File};
use std::io;
use std::path::{Component, Path, PathBuf};

/// Unpacks a .zip. With `strip`, the archive's single top folder is dropped.
pub fn unzip(archive: &Path, dest: &Path, strip: bool) -> Result<()> {
    let mut zip = zip::ZipArchive::new(File::open(archive)?)
        .with_context(|| format!("{} is not a valid zip file", archive.display()))?;
    fs::create_dir_all(dest)?;
    for i in 0..zip.len() {
        let mut entry = zip.by_index(i)?;
        let Some(rel) = entry.enclosed_name() else {
            continue;
        };
        let rel: PathBuf = if strip {
            let mut c = rel.components();
            c.next();
            c.as_path().to_path_buf()
        } else {
            rel
        };
        if rel.as_os_str().is_empty() {
            continue;
        }
        let out = dest.join(&rel);
        if entry.is_dir() {
            fs::create_dir_all(&out)?;
            continue;
        }
        if let Some(p) = out.parent() {
            fs::create_dir_all(p)?;
        }
        let mut f = File::create(&out)?;
        io::copy(&mut entry, &mut f)?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mode = entry.unix_mode().unwrap_or(0o644);
            let exec = mode & 0o111 != 0 || rel.extension().is_none();
            fs::set_permissions(
                &out,
                fs::Permissions::from_mode(if exec { 0o755 } else { 0o644 }),
            )?;
        }
    }
    Ok(())
}

/// Unpacks a .tar.xz / .tar.gz with the system's tar (Windows 10+ ships one
/// that reads both; on Linux it is always there).
pub fn untar(r: &Reporter, archive: &Path, dest: &Path, strip: bool) -> Result<()> {
    fs::create_dir_all(dest)?;
    let mut cmd = Env::default().command(if cfg!(windows) { "tar.exe" } else { "tar" });
    cmd.arg("-xf").arg(archive).arg("-C").arg(dest);
    if strip {
        cmd.arg("--strip-components=1");
    }
    run(r, cmd, &format!("Unpacking {}", archive.display()), |_| {})
}

/// Deletes a folder, also when files in it are read-only (git objects).
pub fn remove_dir(dir: &Path) -> Result<()> {
    if !dir.exists() {
        return Ok(());
    }
    if fs::remove_dir_all(dir).is_ok() {
        return Ok(());
    }
    clear_readonly(dir);
    fs::remove_dir_all(dir).with_context(|| format!("could not delete {}", dir.display()))
}

fn clear_readonly(dir: &Path) {
    let Ok(rd) = fs::read_dir(dir) else { return };
    for e in rd.flatten() {
        let p = e.path();
        if let Ok(m) = fs::symlink_metadata(&p) {
            if m.is_dir() {
                clear_readonly(&p);
            } else {
                let mut perm = m.permissions();
                #[allow(clippy::permissions_set_readonly_false)]
                perm.set_readonly(false);
                let _ = fs::set_permissions(&p, perm);
            }
        }
    }
}

/// Copies a folder tree. `skip(name)` leaves out files and folders by name.
pub fn copy_tree(
    r: &Reporter,
    from: &Path,
    to: &Path,
    skip: &dyn Fn(&str, bool) -> bool,
    count: &mut u64,
) -> Result<()> {
    fs::create_dir_all(to)?;
    for e in fs::read_dir(from).with_context(|| format!("could not read {}", from.display()))? {
        r.check()?;
        let e = e?;
        let name = e.file_name().to_string_lossy().into_owned();
        let ty = e.file_type()?;
        if skip(&name, ty.is_dir()) {
            continue;
        }
        let dest = to.join(&name);
        if ty.is_dir() {
            copy_tree(r, &e.path(), &dest, skip, count)?;
        } else {
            fs::copy(e.path(), &dest).with_context(|| {
                format!(
                    "could not copy {} to {}",
                    e.path().display(),
                    dest.display()
                )
            })?;
            *count += 1;
        }
    }
    Ok(())
}

/// True when `child` is inside `root` (no "..", no absolute part).
pub fn safe_relative(rel: &str) -> Option<PathBuf> {
    let p = Path::new(rel);
    let mut out = PathBuf::new();
    for c in p.components() {
        match c {
            Component::Normal(s) => out.push(s),
            Component::CurDir => {}
            _ => return None,
        }
    }
    if out.as_os_str().is_empty() {
        None
    } else {
        Some(out)
    }
}

pub fn glob_match(name: &str, pattern: &str) -> bool {
    let name = name.to_ascii_lowercase();
    let pattern = pattern.to_ascii_lowercase();
    if let Some(suffix) = pattern.strip_prefix('*') {
        if let Some(mid) = suffix.strip_suffix('*') {
            return name.contains(mid);
        }
        return name.ends_with(suffix);
    }
    name == pattern
}

pub fn write_text(path: &Path, text: &str) -> Result<()> {
    if let Some(p) = path.parent() {
        fs::create_dir_all(p)?;
    }
    fs::write(path, text).with_context(|| format!("could not write {}", path.display()))
}

pub fn read_text(path: &Path) -> Option<String> {
    fs::read_to_string(path).ok().map(|s| s.trim().to_string())
}

pub fn require(path: &Path, what: &str) -> Result<()> {
    if !path.exists() {
        bail!("{what} is missing: {}", path.display());
    }
    Ok(())
}
