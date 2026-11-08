# MintOS26 website

A static, single-page site for MintOS26 and MintDE. Plain HTML, CSS and JavaScript, with no build step and no dependencies. It works on GitHub Pages as-is.

```
website/
├── index.html              the page
├── 404.html                "page not found" page for GitHub Pages
├── .nojekyll               tells GitHub Pages to serve files untouched
├── css/style.css           all styles (dark by default, follows light mode)
├── js/main.js              menu, screenshot tabs, copy buttons, demo Terminal
└── assets/
    ├── favicon.png, apple-touch-icon.png
    ├── icons/              MintDE app icons, resized to 128 px
    └── screenshots/        JPEGs made from test-evidence/ and docs/evidence/
```

## Preview it locally

From the repository root:

```sh
python3 -m http.server 8000 --directory website
```

Then open http://localhost:8000.

## How it is published

The site lives at **https://thejakubruzicka.github.io/MintOS26/** and is published from the `main` branch of [thejakubruzicka/MintOS26](https://github.com/thejakubruzicka/MintOS26).

The workflow in `.github/workflows/deploy-website.yml` republishes it whenever a change under `website/` reaches `main`. You can also run it by hand from the repo's **Actions** tab. GitHub Pages is set to **Settings → Pages → Source: GitHub Actions**.

Things to keep in mind:

- **Repository name.** Links assume the repo is called `MintOS26`. If you rename it, update the GitHub links in `index.html` and the `/MintOS26/` paths in `404.html`.
- **Wallpaper.** The screenshots show the wallpaper from `assets/wallpaper-*.png`, which appears to be Apple's macOS High Sierra wallpaper. If you replace it in MintOS, retake the screenshots and replace the files in `assets/screenshots/` using the same names.

## Updating for a new release

- **Version badge:** the `eyebrow` line near the top of `index.html`.
- **Releases:** add a new `<li>` at the top of the `timeline` list and move the `is-latest` class and the `latest` badge to it.
- **Status:** update the three lists in the `status` section.
- **Demo Terminal:** the `ver` reply and other strings in `js/main.js` are copied from `kernel/user/desktop.c`. Keep them in sync.
- **Build step:** the disk image name in the QEMU command contains the version number.
- **Screenshots:** replace files in `assets/screenshots/`. On macOS you can shrink a PNG into a web-sized JPEG with:

  ```sh
  sips -s format jpeg -s formatOptions 80 -Z 1600 input.png --out website/assets/screenshots/name.jpg
  ```

  If a new image has a different size, update its `width` and `height` in `index.html`.
