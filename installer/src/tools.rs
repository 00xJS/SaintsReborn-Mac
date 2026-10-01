//! The build toolchain: portable LLVM, CMake, Ninja, Git and the Microsoft C++
//! headers and libraries (fetched by xwin). Nothing is installed system-wide;
//! everything lives in one folder that every Saints Reborn install shares.

use crate::config::{self, Archive, Download};
use crate::fsx;
use crate::platform::{self, EXE};
use crate::report::{self, Env, Reporter, run};
use anyhow::{Result, bail};
use std::fs;
use std::path::{Path, PathBuf};

pub struct Toolchain {
    pub llvm_bin: PathBuf,
    pub cmake: PathBuf,
    pub git: PathBuf,
    pub winsysroot: PathBuf,
    /// CMake toolchain file for building Windows programs on Linux.
    pub cross_file: Option<PathBuf>,
    pub env: Env,
}

impl Toolchain {
    pub fn clang(&self) -> PathBuf {
        self.llvm_bin.join(format!("clang{EXE}"))
    }
    pub fn clangxx(&self) -> PathBuf {
        self.llvm_bin.join(format!("clang++{EXE}"))
    }
}

fn tools_root() -> PathBuf {
    platform::data_dir().join("tools")
}

/// Downloads and unpacks one tool unless it is already there.
fn ensure(r: &Reporter, d: &Download) -> Result<PathBuf> {
    let root = tools_root();
    let dir = root.join(d.dir);
    let marker = dir.join(".saintsreborn-complete");
    if fsx::read_text(&marker).as_deref() == Some(d.sha256) {
        return Ok(dir);
    }
    let downloads = root.join("downloads");
    fs::create_dir_all(&downloads)?;
    let file = downloads.join(d.url.rsplit('/').next().unwrap_or("download"));
    r.status(format!(
        "Downloading {} ({} MB)",
        d.name,
        d.size.div_ceil(1 << 20)
    ));
    report::download(r, d.url, &file, Some(d.sha256))?;
    r.status(format!("Unpacking {}", d.name));
    fsx::remove_dir(&dir)?;
    match d.archive {
        Archive::Tar { strip } => fsx::untar(r, &file, &dir, strip)?,
        Archive::Zip { strip } => fsx::unzip(&file, &dir, strip)?,
    }
    fsx::write_text(&marker, d.sha256)?;
    let _ = fs::remove_file(&file);
    Ok(dir)
}

fn find_git(r: &Reporter) -> Result<PathBuf> {
    #[cfg(windows)]
    {
        let on_path = std::env::var_os("PATH")
            .map(|p| {
                std::env::split_paths(&p)
                    .map(|d| d.join("git.exe"))
                    .find(|p| p.is_file())
            })
            .unwrap_or(None);
        if let Some(g) = on_path {
            return Ok(g);
        }
        for base in ["ProgramW6432", "ProgramFiles"] {
            if let Some(b) = std::env::var_os(base) {
                let g = PathBuf::from(b).join("Git\\cmd\\git.exe");
                if g.is_file() {
                    return Ok(g);
                }
            }
        }
        let dir = ensure(r, &config::MINGIT)?;
        let g = dir.join("cmd\\git.exe");
        if !g.is_file() {
            bail!("Git could not be unpacked.");
        }
        Ok(g)
    }
    #[cfg(not(windows))]
    {
        let _ = r;
        platform::which("git").ok_or_else(|| {
            anyhow::anyhow!(
                "Git was not found. Install it with your package manager (for example \
                 'sudo apt install git' or 'sudo pacman -S git') and press Install again."
            )
        })
    }
}

/// Microsoft's C++ library and Windows SDK headers/libraries, laid out the
/// way clang's -Xmicrosoft-windows-sys-root expects.
fn ensure_winsysroot(r: &Reporter, xwin: &Path, accept_license: bool) -> Result<PathBuf> {
    let root = tools_root();
    let out = root.join(format!(
        "winsysroot-{}-{}",
        config::XWIN_CRT_VERSION,
        config::XWIN_SDK_VERSION
    ));
    let marker = out.join(".saintsreborn-complete");
    if marker.exists() {
        return Ok(out);
    }
    if !accept_license {
        bail!(
            "The Microsoft C++ headers and libraries come under Microsoft's Visual Studio license \
             ({}). Tick the box to accept it and press Install again.",
            config::MS_LICENSE_URL
        );
    }
    r.status("Downloading the Microsoft C++ headers and libraries (about 600 MB)");
    fsx::remove_dir(&out)?;
    let cache = root.join("xwin-cache");
    let attempt = |pinned: bool| -> Result<()> {
        let mut cmd = Env::default().command(xwin);
        cmd.args([
            "--accept-license",
            "--log-level",
            "warn",
            "--http-retry",
            "3",
        ])
        .arg("--cache-dir")
        .arg(&cache);
        if pinned {
            cmd.args([
                "--crt-version",
                config::XWIN_CRT_VERSION,
                "--sdk-version",
                config::XWIN_SDK_VERSION,
            ]);
        }
        cmd.args([
            "splat",
            "--use-winsysroot-style",
            "--preserve-ms-arch-notation",
        ]);
        if cfg!(windows) {
            cmd.arg("--disable-symlinks");
        }
        cmd.arg("--output").arg(&out);
        run(
            r,
            cmd,
            "Downloading the Microsoft C++ headers and libraries",
            |_| {},
        )
    };
    if let Err(e) = attempt(true) {
        r.check()?;
        r.log(format!(
            "Pinned versions not available ({e}); using the newest ones."
        ));
        fsx::remove_dir(&out)?;
        attempt(false)?;
    }
    fsx::remove_dir(&cache)?;
    if !out.join("VC").is_dir() {
        bail!("The Microsoft C++ headers and libraries were not unpacked.");
    }
    fsx::write_text(&marker, config::TOOLCHAIN_ID)?;
    Ok(out)
}

/// Every clang call that targets Windows reads these: where the Microsoft
/// headers/libraries are and that lld does the linking. On Windows clang.cfg
/// and clang++.cfg apply to every call; the triple-named files also cover
/// llvm-rc's preprocessor and the cross build on Linux.
/// Linux: Microsoft's headers and libraries assume a file system that ignores
/// case (the SDK includes <ObjBase.h>, the file is objbase.h). clang and
/// lld-link read this overlay, which lists every file and ignores case.
#[cfg(not(windows))]
fn write_case_overlay(winsysroot: &Path) -> Result<PathBuf> {
    fn esc(s: &str) -> String {
        s.replace('\\', "\\\\").replace('"', "\\\"")
    }
    fn walk(dir: &Path, out: &mut String) -> Result<()> {
        let mut first = true;
        for e in fs::read_dir(dir)? {
            let e = e?;
            let meta = fs::symlink_metadata(e.path())?;
            if meta.file_type().is_symlink() {
                continue; // xwin's case symlinks; the overlay replaces them
            }
            let name = esc(&e.file_name().to_string_lossy());
            if !first {
                out.push(',');
            }
            first = false;
            if meta.is_dir() {
                out.push_str(&format!(
                    "{{\"name\":\"{name}\",\"type\":\"directory\",\"contents\":["
                ));
                walk(&e.path(), out)?;
                out.push_str("]}");
            } else {
                let full = esc(&e.path().to_string_lossy());
                out.push_str(&format!(
                    "{{\"name\":\"{name}\",\"type\":\"file\",\"external-contents\":\"{full}\"}}"
                ));
            }
        }
        Ok(())
    }
    let root = esc(&winsysroot.to_string_lossy());
    let mut s = format!(
        "{{\"version\":0,\"case-sensitive\":\"false\",\"roots\":[{{\"name\":\"{root}\",\"type\":\"directory\",\"contents\":["
    );
    walk(winsysroot, &mut s)?;
    s.push_str("]}]}");
    let name = format!(
        "{}.vfs.json",
        winsysroot.file_name().unwrap_or_default().to_string_lossy()
    );
    let path = winsysroot.parent().unwrap_or(winsysroot).join(name);
    fsx::write_text(&path, &s)?;
    Ok(path)
}

fn write_clang_config(llvm_bin: &Path, winsysroot: &Path) -> Result<()> {
    let root = winsysroot.to_string_lossy().replace('\\', "/");
    #[cfg_attr(windows, allow(unused_mut))]
    let mut cfg = format!(
        "-Xmicrosoft-windows-sys-root \"{root}\"\n-fuse-ld=lld\n-Wno-unused-command-line-argument\n"
    );
    #[cfg(not(windows))]
    {
        let overlay = write_case_overlay(winsysroot)?;
        let o = overlay.to_string_lossy();
        cfg += &format!("-ivfsoverlay \"{o}\"\n-Xlinker \"/vfsoverlay:{o}\"\n");
    }
    let mut names = vec![
        "x86_64-pc-windows-msvc.cfg",
        "x86_64-pc-windows-msvc-clang.cfg",
        "x86_64-pc-windows-msvc-clang++.cfg",
    ];
    if cfg!(windows) {
        names.extend(["clang.cfg", "clang++.cfg"]);
    }
    for n in names {
        fsx::write_text(&llvm_bin.join(n), &cfg)?;
    }
    Ok(())
}

#[cfg(not(windows))]
fn write_cross_file(llvm_bin: &Path) -> Result<PathBuf> {
    let b = llvm_bin.to_string_lossy();
    let text = format!(
        "# Builds Windows x64 programs with clang + lld + the xwin headers/libraries.\n\
         set(CMAKE_SYSTEM_NAME Windows)\n\
         set(CMAKE_SYSTEM_PROCESSOR AMD64)\n\
         set(CMAKE_C_COMPILER \"{b}/clang\")\n\
         set(CMAKE_CXX_COMPILER \"{b}/clang++\")\n\
         set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)\n\
         set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)\n\
         set(CMAKE_RC_COMPILER \"{b}/llvm-rc\")\n\
         set(CMAKE_AR \"{b}/llvm-ar\")\n\
         set(CMAKE_RANLIB \"{b}/llvm-ranlib\")\n\
         set(CMAKE_MT \"{b}/llvm-mt\")\n\
         set(CMAKE_LINKER_TYPE LLD)\n\
         # Only the release C runtime is downloaded (no debug libraries).\n\
         set(CMAKE_TRY_COMPILE_CONFIGURATION Release)\n"
    );
    let path = tools_root().join("windows-x64.cmake");
    fsx::write_text(&path, &text)?;
    Ok(path)
}

pub fn ensure_all(r: &Reporter, accept_license: bool) -> Result<Toolchain> {
    let git = find_git(r)?;
    r.log(format!("Git: {}", git.display()));
    let mut dirs = Vec::new();
    for d in config::TOOLS {
        dirs.push(ensure(r, d)?);
    }
    let llvm = &dirs[0];
    let cmake_dir = &dirs[1];
    let ninja_dir = &dirs[2];
    let xwin_dir = &dirs[3];
    let llvm_bin = llvm.join("bin");
    let xwin = xwin_dir.join(format!("xwin{EXE}"));
    let winsysroot = ensure_winsysroot(r, &xwin, accept_license)?;
    write_clang_config(&llvm_bin, &winsysroot)?;

    #[cfg(not(windows))]
    let cross_file = Some(write_cross_file(&llvm_bin)?);
    #[cfg(windows)]
    let cross_file = None;

    let env = Env {
        path_prefix: vec![
            llvm_bin.clone(),
            cmake_dir.join("bin"),
            ninja_dir.clone(),
            git.parent().map(Path::to_path_buf).unwrap_or_default(),
        ],
        vars: vec![],
    };
    let tc = Toolchain {
        cmake: cmake_dir.join("bin").join(format!("cmake{EXE}")),
        llvm_bin,
        git,
        winsysroot,
        cross_file,
        env,
    };
    r.log(format!(
        "Microsoft headers and libraries: {}",
        tc.winsysroot.display()
    ));
    let v = report::capture(tc.env.command(tc.clang()).arg("--version")).unwrap_or_default();
    r.log(format!("Compiler: {}", v.lines().next().unwrap_or("?")));
    if !v.contains(config::LLVM_VERSION) {
        bail!(
            "The downloaded clang does not run ({}).",
            tc.clang().display()
        );
    }
    Ok(tc)
}
