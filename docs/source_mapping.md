# Source hierarchy and restoration

The primary source material is stored in native, byte-preserving ZIP archives to retain exact Vivado/Vitis entity and project paths, including required mixed-case `.v`, `.bd`, `.xci` and project filenames. The public repository uses lowercase path names for navigational directories and read-only source mirrors.

`python tools/prepare_sources.py` extracts native archives without changing their internal file bytes or names. The `.xpr` project then resolves its own original relative source paths and the generated block-design wrapper path.

`src/pl/rtl/` and `src/ps/c/` are lowercase, byte-identical browsing copies for reading only; build from the native restored projects. All source-to-release file mappings and original SHA-256 digests are listed in `source_provenance.csv`.
