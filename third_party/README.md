# Third-party code

Vendored unchanged from the projects' official GitHub tagged releases (source tarballs).
Only the files the build uses are kept.

| Library | Licence | Tag | Tarball | sha256 of the tarball |
|---|---|---|---|---|
| Dear ImGui (`imgui/`) | MIT (`imgui/LICENSE.txt`) | v1.92.9b | https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz | `21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99` |
| MinHook (`minhook/`) | BSD-2-Clause (`minhook/LICENSE.txt`) | v1.3.4 | https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.4.tar.gz | `1aebeae4ca898330c507860acc2fca2eb335fe446a3a2b8444c3bf8b2660a14e` |

From ImGui: the core files, `imconfig.h` and `backends/imgui_impl_dx11.*`. Build-time
options go on the compiler's command line (`build.sh`), not into `imconfig.h`; among them
`IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS`, which leaves out ImGui's ShellExecute "open URL"
handler. From MinHook: `include/`, `src/` and the licence and authors files.
