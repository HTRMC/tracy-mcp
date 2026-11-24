# tracy-mcp

An [MCP](https://modelcontextprotocol.io) server that lets AI agents query
[Tracy profiler](https://github.com/wolfpld/tracy) `.tracy` captures.

Written in C++ and linked directly against Tracy's own server/`Worker` library
(v0.14.1), so it reads captures with the exact same code the Tracy GUI uses - no
binary-format reimplementation, no shelling out to CLI tools. Works with any MCP
client: Claude Code, Codex, Cursor, etc.

## Tools

| Tool | What it answers |
|------|-----------------|
| `capture_info` | Program, duration, zone/thread/frame counts - the overview. |
| `hotspots` | Top zones by total (or self) time. "What is slow?" |
| `zone` | Stats for zones matching a name substring. |
| `compare_captures` | Diff zone times between two captures. "What regressed?" |

All tools take an absolute `path` to a `.tracy` file. Stats (mean/min/max/std,
self-time) match `tracy-csvexport` exactly.

## Build

Needs a C++20 compiler, CMake ≥ 3.16, Ninja, and git. Dependencies (capstone,
zstd, ppqsort, nlohmann/json) are fetched automatically via CPM on first
configure.

```sh
git clone --recurse-submodules <this repo>   # Tracy lives in third_party/tracy
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
# -> build/tracy-mcp(.exe)
cmake --install build --prefix <dir>          # optional: puts tracy-mcp on PATH
```

If you cloned without submodules:
`git submodule update --init` (or the repo vendors Tracy at `third_party/tracy`).

## Wire it up

Use the path to your built binary (`build/tracy-mcp` or `build/tracy-mcp.exe`),
or just `tracy-mcp` if you ran `cmake --install` and it is on PATH.

### Claude Code
```sh
claude mcp add tracy -- /path/to/tracy-mcp.exe
```

### Codex / other clients (stdio)
Point the client at the binary as a stdio MCP server. Example config block:
```json
{
  "mcpServers": {
    "tracy": { "command": "/path/to/tracy-mcp.exe" }
  }
}
```

Then ask things like *"Run hotspots on C:/captures/frame_spike.tracy, top 10 by
self time"* or *"Compare baseline.tracy with today.tracy."*

## Install

Two channels, both backed by per-OS binaries built in CI
(`.github/workflows/release.yml`, triggered by pushing a `v*` tag).

### Claude Code plugin (build it yourself)
This repo is also a plugin marketplace (`.claude-plugin/marketplace.json`). The
plugin runs `tracy-mcp` from PATH, so build + install first
(`cmake --install`), then:
```sh
/plugin marketplace add HTRMC/tracy-mcp
/plugin install tracy-mcp@HTRMC
```

### MCPB bundle (one-click, Claude Desktop)
No build, no toolchain. Grab `tracy-mcp-<os>.mcpb` for your OS from the
[Releases](https://github.com/HTRMC/tracy-mcp/releases) page, then in Claude
Desktop: Settings -> Extensions -> Advanced settings -> Install Extension, and
pick the file.

## Notes

- Captures are cached in memory per path for the server's lifetime. Restart the
  server to pick up a re-capture written to the same path.
- Built and tested against Tracy **0.14.1** (also reads 0.13.1 captures). The `.tracy` format is versioned;
  bump the `third_party/tracy` submodule to match your capture's version.
