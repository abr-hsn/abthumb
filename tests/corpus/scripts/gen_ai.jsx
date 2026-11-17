// gen_ai.jsx -- abthumb test corpus generator for Adobe Illustrator (ExtendScript, ES3).
//
// Driven by run_corpus.ps1 through COM; it predefines these globals before evaluating this file:
//   CORPUS_ROOT   absolute path of tests/corpus           (optional, derived from this file otherwise)
//   CORPUS_FILTER comma separated base names to build     (optional, default: all bases)
// Output:  <root>/generated/{ai,eps,pdf,ref}/...  and  <root>/generated/manifest_ai.csv (+ log_ai.txt)
//
// Never throws to the caller: every file is wrapped in try/catch and failures go to the manifest.
#target illustrator

(function () {
    var ROOT = (typeof CORPUS_ROOT !== "undefined") ? CORPUS_ROOT : new File($.fileName).parent.parent.fsName;
    ROOT = ROOT.replace(/\\/g, "/");
    var FILTER = (typeof CORPUS_FILTER !== "undefined" && CORPUS_FILTER) ? ("," + CORPUS_FILTER + ",") : "";
    var GEN = ROOT + "/generated";
    var APPVER = app.version;

    var manifest = new File(GEN + "/manifest_ai.csv");
    var logf = new File(GEN + "/log_ai.txt");
    manifest.encoding = "UTF-8"; logf.encoding = "UTF-8";
    manifest.open("w"); manifest.write("file,app,app_version,base,variant,options,width_pt_or_px,height,expected_has_preview,ok,error\n");
    logf.open("w");

    function log(s) { logf.writeln((new Date()).toTimeString().substr(0, 8) + " " + s); logf.close(); logf.open("a"); }
    function q(s) { s = String(s === undefined || s === null ? "" : s).replace(/[\r\n]+/g, " "); return '"' + s.replace(/"/g, '""') + '"'; }
    function row(file, base, variant, options, w, h, prev, ok, err) {
        manifest.writeln([q(file), "Illustrator", q(APPVER), q(base), q(variant), q(options), q(w), q(h), prev, ok ? "yes" : "no", q(err || "")].join(","));
        manifest.close(); manifest.open("a");
        log((ok ? "OK   " : "FAIL ") + file + (err ? "  :: " + err : ""));
    }

    // ---- environment -------------------------------------------------------------------------
    var origUI = app.userInteractionLevel;
    app.userInteractionLevel = UserInteractionLevel.DONTDISPLAYALERTS;

    // ---- compatibility enum probe ------------------------------------------------------------
    var COMPAT = [];   // [name, value] for every value the DOM exposes
    var cand = ["ILLUSTRATOR3", "JAPANESEVERSION3", "ILLUSTRATOR8", "ILLUSTRATOR9", "ILLUSTRATOR10", "ILLUSTRATOR11",
        "ILLUSTRATOR12", "ILLUSTRATOR13", "ILLUSTRATOR14", "ILLUSTRATOR15", "ILLUSTRATOR16", "ILLUSTRATOR17",
        "ILLUSTRATOR18", "ILLUSTRATOR19", "ILLUSTRATOR20", "ILLUSTRATOR21", "ILLUSTRATOR22", "ILLUSTRATOR23",
        "ILLUSTRATOR24", "ILLUSTRATOR25", "ILLUSTRATOR26", "ILLUSTRATOR27", "ILLUSTRATOR28", "ILLUSTRATOR29",
        "ILLUSTRATOR30", "ILLUSTRATOR"];
    var seen = {};
    for (var ci = 0; ci < cand.length; ci++) {
        try { var cv = Compatibility[cand[ci]]; if (cv !== undefined) { COMPAT.push([cand[ci], cv]); seen[cand[ci]] = 1; } } catch (e) { }
    }
    try { for (var k in Compatibility) { if (!seen[k]) { COMPAT.push([k, Compatibility[k]]); } } } catch (e) { }
    var names = []; for (var i = 0; i < COMPAT.length; i++) names.push(COMPAT[i][0]);
    log("app version " + APPVER + "; Compatibility values exposed: " + names.join(","));
    function compatByName(n) { for (var i = 0; i < COMPAT.length; i++) if (COMPAT[i][0] == n) return COMPAT[i][1]; return null; }
    var CUR = compatByName("ILLUSTRATOR24"); // highest value exposed == the "current" format for this DOM

    // ---- colour / geometry helpers -----------------------------------------------------------
    function rgb(r, g, b) { var c = new RGBColor(); c.red = r; c.green = g; c.blue = b; return c; }
    function cmyk(c, m, y, k) { var o = new CMYKColor(); o.cyan = c; o.magenta = m; o.yellow = y; o.black = k; return o; }
    // colour in the document colour space from an RGB triple
    function mk(isCMYK, r, g, b) {
        if (!isCMYK) return rgb(r, g, b);
        var k = 1 - Math.max(r, g, b) / 255; if (k >= 1) return cmyk(0, 0, 0, 100);
        return cmyk(Math.round(100 * (1 - r / 255 - k) / (1 - k)), Math.round(100 * (1 - g / 255 - k) / (1 - k)),
            Math.round(100 * (1 - b / 255 - k) / (1 - k)), Math.round(100 * k));
    }
    function style(it, fill, stroke, sw) {
        if (fill) { it.filled = true; it.fillColor = fill; } else it.filled = false;
        if (stroke) { it.stroked = true; it.strokeColor = stroke; it.strokeWidth = sw || 1; } else it.stroked = false;
        return it;
    }
    function mkGradient(doc, name, type, c1, c2) {
        var g = doc.gradients.add(); g.name = name; g.type = type;
        g.gradientStops[0].rampPoint = 0; g.gradientStops[0].color = c1;
        g.gradientStops[1].rampPoint = 100; g.gradientStops[1].color = c2;
        return g;
    }
    function gradColor(g, x, y, len, angle) {
        var gc = new GradientColor(); gc.gradient = g; gc.origin = [x, y]; gc.angle = angle || 0; gc.length = len; return gc;
    }
    function findFont() {
        var n = ["ArialMT", "Arial-BoldMT", "MyriadPro-Regular", "Myriad-Pro"];
        for (var i = 0; i < n.length; i++) { try { return app.textFonts.getByName(n[i]); } catch (e) { } }
        return null;
    }
    function addText(layer, str, x, y, size, color) {
        var tf = layer.textFrames.pointText([x, y]);
        tf.contents = str;
        var ca = tf.textRange.characterAttributes;
        var f = findFont(); if (f) { try { ca.textFont = f; } catch (e) { } }
        ca.size = size; ca.fillColor = color;
        return tf;
    }
    var seed = 12345;
    function rnd() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; }  // deterministic

    // artboard-relative scene builder: L,T = top-left (doc coords, y up), W,H size
    function scene(doc, layer, L, T, W, H, isCMYK, label) {
        var bg = layer.pathItems.rectangle(T, L, W, H);
        style(bg, mk(isCMYK, 245, 245, 240), mk(isCMYK, 60, 60, 60), 4);
        var r1 = layer.pathItems.rectangle(T - H * 0.06, L + W * 0.06, W * 0.38, H * 0.22);
        style(r1, mk(isCMYK, 220, 60, 50), mk(isCMYK, 90, 20, 20), 6);
        var e1 = layer.pathItems.ellipse(T - H * 0.06, L + W * 0.54, W * 0.38, H * 0.22);
        style(e1, mk(isCMYK, 40, 120, 220), mk(isCMYK, 10, 40, 100), 3);
        var lin = mkGradient(doc, "lin_" + label, GradientType.LINEAR, mk(isCMYK, 255, 200, 0), mk(isCMYK, 200, 0, 120));
        var r2 = layer.pathItems.rectangle(T - H * 0.34, L + W * 0.06, W * 0.38, H * 0.2);
        style(r2, gradColor(lin, L + W * 0.06, T - H * 0.44, W * 0.38, 0), null);
        var rad = mkGradient(doc, "rad_" + label, GradientType.RADIAL, mk(isCMYK, 255, 255, 255), mk(isCMYK, 0, 140, 90));
        var e2 = layer.pathItems.ellipse(T - H * 0.34, L + W * 0.54, W * 0.38, H * 0.2);
        style(e2, gradColor(rad, L + W * 0.73, T - H * 0.44, W * 0.19, 0), mk(isCMYK, 0, 60, 40), 2);
        var st = layer.pathItems.star(L + W * 0.25, T - H * 0.68, Math.min(W, H) * 0.14, Math.min(W, H) * 0.06, 5);
        style(st, mk(isCMYK, 250, 190, 20), mk(isCMYK, 120, 80, 0), 4);
        var tsz = Math.max(8, Math.min(W / 12, H / 8));
        addText(layer, "abthumb corpus", L + W * 0.06, T - H * 0.9, tsz, mk(isCMYK, 20, 20, 60));
        addText(layer, label, L + W * 0.06, T - H * 0.96, tsz * 0.5, mk(isCMYK, 80, 80, 80));
        return { star: st, bg: bg, r1: r1, e1: e1 };
    }

    // ---- base documents ----------------------------------------------------------------------
    var BASES = [];
    function defBase(name, build) { BASES.push({ name: name, build: build }); }

    defBase("basic_rgb", function () {
        var d = app.documents.add(DocumentColorSpace.RGB, 800, 600);
        var a = d.artboards[0].artboardRect; scene(d, d.layers[0], a[0], a[1], 800, 600, false, "basic_rgb");
        return d;
    });
    defBase("basic_cmyk", function () {
        var d = app.documents.add(DocumentColorSpace.CMYK, 595, 842);
        var a = d.artboards[0].artboardRect; var L = a[0], T = a[1];
        scene(d, d.layers[0], L, T, 595, 842, true, "basic_cmyk");
        var spot = d.spots.add(); spot.name = "PANTONE 485 C (abthumb)"; spot.colorType = ColorModel.SPOT; spot.color = cmyk(0, 95, 100, 0);
        var sc = new SpotColor(); sc.spot = spot; sc.tint = 100;
        var sr = d.layers[0].pathItems.rectangle(T - 842 * 0.76, L + 595 * 0.55, 595 * 0.38, 842 * 0.1);
        style(sr, sc, cmyk(0, 0, 0, 100), 2);
        var sc2 = new SpotColor(); sc2.spot = spot; sc2.tint = 40;
        var sr2 = d.layers[0].pathItems.rectangle(T - 842 * 0.87, L + 595 * 0.55, 595 * 0.38, 842 * 0.05);
        style(sr2, sc2, null);
        return d;
    });
    defBase("multi_artboard", function () {
        var d = app.documents.add(DocumentColorSpace.RGB, 400, 300);
        var a0 = d.artboards[0].artboardRect; var L = a0[0], T = a0[1];
        d.artboards[0].name = "AB1";
        var ab2 = d.artboards.add([L + 450, T, L + 450 + 600, T - 450]); ab2.name = "AB2";
        var ab3 = d.artboards.add([L + 1100, T, L + 1100 + 250, T - 250]); ab3.name = "AB3";
        var defs = [
            { r: [L, T, L + 400, T - 300], c: [230, 60, 50], n: "1", shape: "rect" },
            { r: [L + 450, T, L + 1050, T - 450], c: [50, 170, 80], n: "2", shape: "ellipse" },
            { r: [L + 1100, T, L + 1350, T - 250], c: [50, 90, 220], n: "3", shape: "star" }];
        var lay = d.layers[0];
        for (var i = 0; i < defs.length; i++) {
            var r = defs[i].r, w = r[2] - r[0], h = r[1] - r[3], col = rgb(defs[i].c[0], defs[i].c[1], defs[i].c[2]);
            var it;
            if (defs[i].shape == "rect") it = lay.pathItems.rectangle(r[1] - h * 0.1, r[0] + w * 0.1, w * 0.8, h * 0.8);
            else if (defs[i].shape == "ellipse") it = lay.pathItems.ellipse(r[1] - h * 0.1, r[0] + w * 0.1, w * 0.8, h * 0.8);
            else it = lay.pathItems.star(r[0] + w / 2, r[1] - h / 2, w * 0.4, w * 0.18, 5);
            style(it, col, rgb(0, 0, 0), 4);
            addText(lay, defs[i].n, r[0] + w * 0.42, r[1] - h * 0.62, Math.min(w, h) * 0.45, rgb(255, 255, 255));
        }
        d.artboards.setActiveArtboardIndex(0);
        return d;
    });
    defBase("transparency", function () {
        var d = app.documents.add(DocumentColorSpace.RGB, 600, 400);
        var a = d.artboards[0].artboardRect; var L = a[0], T = a[1], lay = d.layers[0];
        var bg = lay.pathItems.rectangle(T, L, 600, 400); style(bg, rgb(255, 255, 255), null);
        var stripe = lay.pathItems.rectangle(T - 150, L, 600, 100); style(stripe, rgb(40, 40, 40), null);
        var a1 = lay.pathItems.ellipse(T - 40, L + 60, 220, 220); style(a1, rgb(230, 30, 30), null); a1.opacity = 50;
        var a2 = lay.pathItems.ellipse(T - 40, L + 180, 220, 220); style(a2, rgb(30, 60, 230), null); a2.opacity = 50;
        var m = lay.pathItems.rectangle(T - 60, L + 330, 200, 160); style(m, rgb(250, 200, 30), null); m.blendingMode = BlendModes.MULTIPLY;
        var s = lay.pathItems.ellipse(T - 120, L + 400, 160, 160); style(s, rgb(30, 200, 200), null); s.blendingMode = BlendModes.SCREEN;
        var ds = lay.pathItems.rectangle(T - 270, L + 60, 200, 90); style(ds, rgb(240, 240, 240), rgb(100, 100, 100), 2);
        var fx = "skipped (applyEffect unavailable)";
        try {
            var xml = '<LiveEffect name="Adobe Drop Shadow"><Dict data="R mode 2 R opacity 0.75 R blur 5 R xoffset 7 R yoffset 7 B darken 0 R darkness 1 B useGlobalLight 0 R color_r 0 R color_g 0 R color_b 0 "/></LiveEffect>';
            ds.applyEffect(xml); fx = "applied";
        } catch (e) { fx = "skipped: " + e; }
        log("transparency drop shadow: " + fx);
        addText(lay, "50% / Multiply / Screen", L + 300, T - 330, 28, rgb(20, 20, 20));
        return d;
    });
    defBase("raster", function () {
        var d = app.documents.add(DocumentColorSpace.RGB, 600, 400);
        var a = d.artboards[0].artboardRect; var L = a[0], T = a[1], lay = d.layers[0];
        var g = mkGradient(d, "ras_g", GradientType.LINEAR, rgb(255, 80, 0), rgb(0, 80, 255));
        var r = lay.pathItems.rectangle(T - 40, L + 40, 520, 320); style(r, gradColor(g, L + 40, T - 200, 520, 0), null);
        var ro = new RasterizeOptions(); ro.resolution = 150; ro.antiAliasingMethod = AntiAliasingMethod.ARTOPTIMIZED;
        d.rasterize(r, r.geometricBounds, ro);
        var e = lay.pathItems.ellipse(T - 100, L + 120, 200, 200); style(e, rgb(255, 255, 255), rgb(0, 0, 0), 6);
        addText(lay, "raster + vector", L + 330, T - 220, 40, rgb(255, 255, 255));
        return d;
    });
    defBase("heavy", function () {
        var d = app.documents.add(DocumentColorSpace.RGB, 3000, 3000);
        var a = d.artboards[0].artboardRect; var L = a[0], T = a[1], lay = d.layers[0];
        seed = 777;
        for (var i = 0; i < 3000; i++) {
            var w = 8 + rnd() * 50, h = 8 + rnd() * 50, x = L + rnd() * (3000 - w), y = T - rnd() * (3000 - h);
            var it = (i % 2) ? lay.pathItems.ellipse(y, x, w, h) : lay.pathItems.rectangle(y, x, w, h);
            style(it, rgb(Math.floor(rnd() * 256), Math.floor(rnd() * 256), Math.floor(rnd() * 256)), (i % 5 == 0) ? rgb(0, 0, 0) : null, 1);
        }
        return d;
    });
    defBase("tiny", function () {
        var d = app.documents.add(DocumentColorSpace.RGB, 20, 20);
        var a = d.artboards[0].artboardRect;
        var e = d.layers[0].pathItems.ellipse(a[1] - 3, a[0] + 3, 14, 14); style(e, rgb(220, 40, 40), rgb(0, 0, 0), 1);
        return d;
    });

    // ---- save helpers ------------------------------------------------------------------------
    function fileOK(f) { return f.exists && f.length > 0; }
    // run one save; fn(f) performs doc.saveAs/exportFile
    function doSave(rel, base, variant, options, w, h, prev, fn) {
        var f = new File(GEN + "/" + rel);
        try {
            if (f.exists) f.remove();
            fn(f);
            if (!fileOK(f)) throw new Error("file not written");
            row(rel, base, variant, options, w, h, prev, true, "");
        } catch (e) {
            row(rel, base, variant, options, w, h, prev, false, e);
        }
    }

    function saveAIVariants(doc, base, w, h, full) {
        function ai(tag, desc, prev, setter) {
            doSave("ai/" + base + "__ai_" + tag + ".ai", base, "ai_" + tag, desc, w, h, prev, function (f) {
                var o = new IllustratorSaveOptions(); setter(o); doc.saveAs(f, o);
            });
        }
        if (full) {
            for (var i = 0; i < COMPAT.length; i++) {
                (function (nm, val) {
                    // PDF-compat stream only exists in AI9+ ; older formats are PostScript-ish
                    var old = (nm == "ILLUSTRATOR3" || nm == "JAPANESEVERSION3" || nm == "ILLUSTRATOR8");
                    ai(nm, "compatibility=" + nm + " (defaults otherwise)", old ? "unknown" : "yes", function (o) { o.compatibility = val; });
                })(COMPAT[i][0], COMPAT[i][1]);
            }
        }
        ai("cur", "default options (compatibility=" + (CUR === null ? "default" : "ILLUSTRATOR24") + ", pdfCompatible=true, compressed=true)", "yes", function (o) { });
        ai("cur_nopdf", "pdfCompatible=false", "unknown", function (o) { o.pdfCompatible = false; });
        if (full) {
            ai("cur_nocomp", "compressed=false", "yes", function (o) { o.compressed = false; });
            ai("cur_noicc", "embedICCProfile=false", "yes", function (o) { o.embedICCProfile = false; });
            ai("cur_nopdf_nocomp", "pdfCompatible=false,compressed=false", "unknown", function (o) { o.pdfCompatible = false; o.compressed = false; });
        }
    }

    function saveEPSVariants(doc, base, w, h, matrix) {
        var comps = matrix ? [["ILLUSTRATOR8", compatByName("ILLUSTRATOR8")], ["ILLUSTRATOR10", compatByName("ILLUSTRATOR10")],
            ["ILLUSTRATOR16", compatByName("ILLUSTRATOR16")], ["cur", CUR]] : [["cur", CUR]];
        var prevs = matrix ? [["NONE", EPSPreview.None, "no"], ["BWTIFF", EPSPreview.BWTIFF, "yes"], ["COLORTIFF", EPSPreview.COLORTIFF, "yes"],
            ["TRANSPARENTCOLORTIFF", EPSPreview.TRANSPARENTCOLORTIFF, "yes"]] : [["COLORTIFF", EPSPreview.COLORTIFF, "yes"]];
        for (var i = 0; i < comps.length; i++) for (var j = 0; j < prevs.length; j++) {
            (function (cn, cv, pn, pv, pe) {
                doSave("eps/" + base + "__eps_" + cn + "_" + pn + ".eps", base, "eps_" + cn + "_" + pn, "compatibility=" + cn + ",preview=" + pn, w, h, pe, function (f) {
                    var o = new EPSSaveOptions();
                    if (cv !== null) o.compatibility = cv;
                    o.preview = pv; o.embedAllFonts = true; o.saveMultipleArtboards = false;
                    doc.saveAs(f, o);
                });
            })(comps[i][0], comps[i][1], prevs[j][0], prevs[j][1], prevs[j][2]);
        }
    }

    function savePDFVariants(doc, base, w, h) {
        var presets = [["default", "[Illustrator Default]"], ["small", "[Smallest File Size]"]];
        for (var i = 0; i < presets.length; i++) for (var e = 0; e < 2; e++) {
            (function (pn, pv, edit) {
                var tag = pn + "_" + (edit ? "edit" : "noedit");
                doSave("pdf/" + base + "__pdf_" + tag + ".pdf", base, "pdf_" + tag, "preset=" + pv + ",preserveEditability=" + edit, w, h, "yes", function (f) {
                    var o = new PDFSaveOptions(); o.pDFPreset = pv; o.preserveEditability = edit; o.viewAfterSaving = false;
                    doc.saveAs(f, o);
                });
            })(presets[i][0], presets[i][1], e == 0);
        }
    }

    function saveRefs(doc, base, W, H) {
        var longE = Math.max(W, H), sizes = [1024, 256];
        for (var i = 0; i < sizes.length; i++) {
            (function (px) {
                var sc = px / longE * 100;
                var ow = Math.round(W * px / longE), oh = Math.round(H * px / longE);
                doSave("ref/" + base + "__ai_" + px + ".png", base, "ref_png_" + px, "PNG24 artboard1 clip aa opaque-white scale=" + sc.toFixed(3) + "%", ow, oh, "yes", function (f) {
                    var o = new ExportOptionsPNG24();
                    o.artBoardClipping = true; o.antiAliasing = true; o.transparency = false;
                    o.matte = true; o.matteColor = rgb(255, 255, 255);
                    o.horizontalScale = sc; o.verticalScale = sc;
                    doc.artboards.setActiveArtboardIndex(0);
                    doc.exportFile(f, ExportType.PNG24, o);
                });
            })(sizes[i]);
        }
    }

    // ---- main loop ---------------------------------------------------------------------------
    try {
        for (var bi = 0; bi < BASES.length; bi++) {
            var B = BASES[bi];
            if (FILTER && FILTER.indexOf("," + B.name + ",") < 0) continue;
            var doc = null;
            log("=== building " + B.name);
            try {
                doc = B.build();
                var ab = doc.artboards[0].artboardRect, W = ab[2] - ab[0], H = ab[1] - ab[3];
                var heavy = (B.name == "heavy"), epsMatrix = (B.name == "basic_rgb" || B.name == "basic_cmyk");
                var wantPDF = (B.name == "basic_rgb" || B.name == "basic_cmyk" || B.name == "multi_artboard");
                // references first (document still untouched by lossy saves)
                saveRefs(doc, B.name, W, H);
                saveAIVariants(doc, B.name, W, H, !heavy);
                saveEPSVariants(doc, B.name, W, H, epsMatrix);
                if (wantPDF) savePDFVariants(doc, B.name, W, H);
            } catch (e) {
                row("(base " + B.name + ")", B.name, "build", "", "", "", "unknown", false, e);
            }
            try { if (doc) doc.close(SaveOptions.DONOTSAVECHANGES); } catch (e) { log("close failed: " + e); }
        }
    } finally {
        try { app.userInteractionLevel = origUI; } catch (e) { }
        manifest.close(); logf.writeln("done"); logf.close();
    }
    return "gen_ai done";
})();
