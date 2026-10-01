// Windows: the Saints Reborn icon on the exe.
fn main() {
    println!("cargo:rerun-if-changed=../project/res/SaintsReborn.ico");
    #[cfg(windows)]
    {
        let mut res = winresource::WindowsResource::new();
        res.set_icon("../project/res/SaintsReborn.ico");
        res.set("ProductName", "Saints Reborn Setup");
        res.set("FileDescription", "Saints Reborn Setup");
        if let Err(e) = res.compile() {
            println!("cargo:warning=no icon: {e}");
        }
    }
}
