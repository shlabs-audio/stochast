# Empiria — VCV Rack Library submission checklist

The Empiria suite consists of five plugins (Polis, Methods, Epi,
Space, Decisions) under the brand "SHLabs". Each must be submitted
to the VCV Rack Library individually because the Library indexes by
plugin slug, not by brand.

Library guidelines:
- <https://vcvrack.com/manual/PluginGuide>
- <https://github.com/VCVRack/library> (submission repository)

## One-time pre-submission steps

1. **Publish source on GitHub.** Each plugin must have a public,
   GPL-compatible source URL. The current `plugin.json` files point
   to <https://github.com/kevinschoenholzer/empiria> — update the
   five `sourceUrl` / `manualUrl` / `changelogUrl` fields if you
   publish under a different repo path. Each plugin subdirectory
   (Polis, Methods, Epi, Space, Decisions) can live under that one
   monorepo; the Library tooling reads the `plugin.json` at the
   plugin root, not the repo root.

2. **Create a `CHANGELOG.md`** at the repo root listing version
   `2.0.0` with the first-release feature summary. This file is
   linked from every plugin's `changelogUrl`.

3. **Generate one screenshot per module** at the highest zoom
   level VCV Rack supports, save under `screenshots/<plugin>/`
   (e.g. `screenshots/Methods/Frame.png`). Each module needs its
   own PNG, named after the module's `slug`. The Library tooling
   uses these on the catalogue page.

4. **Build for all three target platforms.** Run

   ```bash
   RACK_DIR=$HOME/Rack-SDK make dist
   ```

   inside each of the five plugin directories. This produces a
   `dist/<slug>-<version>-<arch>.vcvplugin` archive per platform.
   Build at minimum for `mac-arm64`, `mac-x64`, `win-x64`, and
   `lin-x64`. Cross-compilation isn't supported by the Rack
   SDK — use the official build VMs if you don't have access to
   the target machines.

5. **Test each `.vcvplugin` locally** by extracting it into
   `~/Library/Application Support/Rack2/plugins-mac-arm64/`
   (or the equivalent on your platform) and confirming each
   module loads, renders correctly, and processes voltages.

## Per-plugin submission

The submission flow is one pull request per plugin against
`VCVRack/library`. For each of the five plugins:

1. Fork <https://github.com/VCVRack/library> on GitHub.
2. Edit `repos.json` and add an entry:

   ```json
   {
     "name": "SHLabs Methods",
     "slug": "SHLabs-Methods",
     "url": "https://github.com/kevinschoenholzer/empiria",
     "subdir": "Methods",
     "tag": "v2.0.0"
   }
   ```

   The `subdir` field is necessary because all five plugins live
   in the same monorepo. Without it the Library build would only
   find the first plugin.

3. Tag the source repository with `v2.0.0` so the Library build
   has a stable target:

   ```bash
   git tag -a v2.0.0 -m "Empiria 2.0.0 — first Library release"
   git push origin v2.0.0
   ```

4. Open the PR titled, for example, *"Add SHLabs Methods"*. The
   Library maintainers run the cross-platform build, audit the
   plugin.json metadata, and merge after the build succeeds.

5. Repeat for the other four plugins.

## Metadata that's already complete

Each `plugin.json` already has:

- `slug`, `name`, `version`, `license` (GPL-3.0-or-later), `brand`,
  `author`, `authorEmail`, `authorUrl`
- `pluginUrl`, `manualUrl`, `sourceUrl`, `changelogUrl`
- Per-module: `slug`, `name`, `description` (one paragraph),
  `tags` (drawn from VCV's official taxonomy)

The only fields intentionally left empty are `donateUrl` (no donation
page exists yet).

## Outstanding items

- [ ] Publish the source on GitHub at the URL listed above (or
      adjust the five `plugin.json` files if you publish under a
      different path).
- [ ] Write `CHANGELOG.md` at repo root.
- [ ] Capture one screenshot per module.
- [ ] Cross-compile or VM-build for the four target platforms.
- [ ] Open five PRs against `VCVRack/library`.

After the first PR merges, future updates require only:

- Bumping the `"version"` field in `plugin.json`.
- Tagging the new release (e.g. `v2.0.1`).
- Editing the corresponding entry in `repos.json` to point to the
  new tag.

The library re-builds automatically; no PR needed for downstream
version bumps.
