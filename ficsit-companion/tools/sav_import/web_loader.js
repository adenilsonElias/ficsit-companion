// ficsit-companion web bundle of @etothepii/satisfactory-file-parser.
//
// PLACEHOLDER. This file is intentionally a stub. To produce the real
// bundle that the web build loads at runtime:
//
//   cd ficsit-companion/tools/sav_import
//   npm install
//   npx esbuild web_loader_entry.js \
//       --bundle --format=iife --global-name=__ficsitParseSavBundle \
//       --outfile=web_loader.js
//
// (Provide a `web_loader_entry.js` that imports the parser and attaches
//  a function `window.__ficsitParseSav(arrayBuffer): Promise<string>`.)
//
// The web build expects the loaded script to populate
// `window.__ficsitParseSav`. Until this is generated, the "Import .sav"
// button in the web build will reject .sav files and only accept
// pre-parsed wrapper JSON.

window.__ficsitParseSav = window.__ficsitParseSav || function(_buf) {
    return Promise.reject(new Error(
        "web_loader.js is a stub. Run wrapper.js on the .sav file outside "
        + "the browser and pass the resulting JSON instead, or bundle the "
        + "parser per the comments at the top of this file."));
};
