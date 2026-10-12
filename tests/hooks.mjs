/*
 * TB-3PO's ui.js imports Schwung's shared library by its DEVICE path, and it
 * is run here unmodified. This hook points those imports at a Schwung
 * checkout's src/ (SCHWUNG_ROOT, else ../schwung beside this repository) (TB-3PO's own tests do the same against a sibling checkout), or at
 * the shared/ named by TR_SHARED.
 */
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const DEVICE = "/data/UserData/schwung/";
const SRC = process.env.TR_SHARED ? path.dirname(path.resolve(process.env.TR_SHARED))
          : path.join(process.env.SCHWUNG_ROOT || path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../../schwung"), "src");

export function resolve(specifier, context, next) {
    if (specifier.startsWith(DEVICE))
        return { url: pathToFileURL(path.join(SRC, specifier.slice(DEVICE.length))).href, shortCircuit: true };
    return next(specifier, context);
}
