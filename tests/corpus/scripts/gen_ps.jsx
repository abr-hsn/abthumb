// gen_ps.jsx -- abthumb test corpus generator for Adobe Photoshop (ExtendScript, ES3).
//
// Driven by run_corpus.ps1, which predefines these globals before evaluating this file:
//   CORPUS_ROOT   absolute path of tests/corpus          (optional, derived from this file otherwise)
//   CORPUS_FILTER comma separated base names to build    (optional, default: all bases)
// Output: <root>/generated/{psd,psb,eps,pdf,ref}/... and <root>/generated/manifest_ps.csv (+ log_ps.txt)
//
// Every file is wrapped in try/catch; failures are recorded in the manifest and the run continues.
// Preferences changed (rulerUnits, typeUnits, maximizeCompatibility, displayDialogs) are restored in finally.
#target photoshop

(function () {
    var ROOT = (typeof CORPUS_ROOT !== "undefined") ? CORPUS_ROOT : new File($.fileName).parent.parent.fsName;
    ROOT = ROOT.replace(/\\/g, "/");
    var FILTER = (typeof CORPUS_FILTER !== "undefined" && CORPUS_FILTER) ? ("," + CORPUS_FILTER + ",") : "";
    var GEN = ROOT + "/generated";
    var APPVER = app.version;

    var manifest = new File(GEN + "/manifest_ps.csv");
    var logf = new File(GEN + "/log_ps.txt");
    manifest.encoding = "UTF-8"; logf.encoding = "UTF-8";
    manifest.open("w"); manifest.write("file,app,app_version,base,variant,options,width_pt_or_px,height,expected_has_preview,ok,error\n");
    logf.open("w");

    function log(s) { logf.writeln((new Date()).toTimeString().substr(0, 8) + " " + s); logf.close(); logf.open("a"); }
    function q(s) { s = String(s === undefined || s === null ? "" : s).replace(/[\r\n]+/g, " "); return '"' + s.replace(/"/g, '""') + '"'; }
    function row(file, base, variant, options, w, h, prev, ok, err) {
        manifest.writeln([q(file), "Photoshop", q(APPVER), q(base), q(variant), q(options), q(w), q(h), prev, ok ? "yes" : "no", q(err || "")].join(","));
        manifest.close(); manifest.open("a");
        log((ok ? "OK   " : "FAIL ") + file + (err ? "  :: " + err : ""));
    }

    function cTID(s) { return charIDToTypeID(s); }
    function sTID(s) { return stringIDToTypeID(s); }

    // ---- preferences (saved / restored) ------------------------------------------------------
    var origRuler = app.preferences.rulerUnits, origType = app.preferences.typeUnits, origDlg = app.displayDialogs, origMax = null;
    try { origMax = app.preferences.maximizeCompatibility; } catch (e) { log("maximizeCompatibility not readable: " + e); }
    log("Photoshop " + APPVER + "; original maximizeCompatibility=" + origMax);
    app.displayDialogs = DialogModes.NO;
    app.preferences.rulerUnits = Units.PIXELS;
    app.preferences.typeUnits = TypeUnits.PIXELS;

    function setMaxCompat(on) { app.preferences.maximizeCompatibility = on ? QueryStateType.ALWAYS : QueryStateType.NEVER; }

    // ---- drawing helpers ---------------------------------------------------------------------
    function rgb(r, g, b) { var c = new SolidColor(); c.rgb.red = r; c.rgb.green = g; c.rgb.blue = b; return c; }
    var _font = undefined;
    function findFont() {
        if (_font !== undefined) return _font;
        _font = null;
        var want = ["ArialMT", "Arial", "MyriadPro-Regular", "Arial-BoldMT"];
        try {
            for (var w = 0; w < want.length && !_font; w++)
                for (var i = 0; i < app.fonts.length; i++) if (app.fonts[i].postScriptName == want[w]) { _font = want[w]; break; }
            if (!_font) for (var j = 0; j < app.fonts.length; j++) if (/Arial|Myriad/i.test(app.fonts[j].name)) { _font = app.fonts[j].postScriptName; break; }
        } catch (e) { }
        log("text font = " + _font);
        return _font;
    }
    function newLayer(doc, name) { var l = doc.artLayers.add(); l.name = name; return l; }
    function selRect(doc, x1, y1, x2, y2) { doc.selection.select([[x1, y1], [x2, y1], [x2, y2], [x1, y2]]); }
    function selEllipse(x1, y1, x2, y2) {
        var d = new ActionDescriptor(), r = new ActionReference(); r.putProperty(cTID("Chnl"), cTID("fsel")); d.putReference(cTID("null"), r);
        var e = new ActionDescriptor();
        e.putUnitDouble(cTID("Top "), cTID("#Pxl"), y1); e.putUnitDouble(cTID("Left"), cTID("#Pxl"), x1);
        e.putUnitDouble(cTID("Btom"), cTID("#Pxl"), y2); e.putUnitDouble(cTID("Rght"), cTID("#Pxl"), x2);
        d.putObject(cTID("T   "), cTID("Elps"), e); executeAction(cTID("setd"), d, DialogModes.NO);
    }
    function fillSel(doc, c) { doc.selection.fill(c, ColorBlendMode.NORMAL, 100, false); }
    function rgbDesc(c) {
        var o = new ActionDescriptor(); o.putDouble(cTID("Rd  "), c[0]); o.putDouble(cTID("Grn "), c[1]); o.putDouble(cTID("Bl  "), c[2]); return o;
    }
    // linear gradient fill (Action Manager) into the current selection / layer
    function gradient(c1, c2, x1, y1, x2, y2) {
        var d = new ActionDescriptor();
        var f = new ActionDescriptor(); f.putUnitDouble(cTID("Hrzn"), cTID("#Pxl"), x1); f.putUnitDouble(cTID("Vrtc"), cTID("#Pxl"), y1);
        d.putObject(cTID("From"), cTID("Pnt "), f);
        var t = new ActionDescriptor(); t.putUnitDouble(cTID("Hrzn"), cTID("#Pxl"), x2); t.putUnitDouble(cTID("Vrtc"), cTID("#Pxl"), y2);
        d.putObject(cTID("T   "), cTID("Pnt "), t);
        d.putEnumerated(cTID("Type"), cTID("GrdT"), cTID("Lnr "));
        d.putBoolean(cTID("Dthr"), false);
        var g = new ActionDescriptor(); g.putString(cTID("Nm  "), "abthumb"); g.putEnumerated(cTID("GrdF"), cTID("GrdF"), cTID("CstS")); g.putDouble(cTID("Intr"), 4096);
        var cl = new ActionList(), cs = [c1, c2], loc = [0, 4096];
        for (var i = 0; i < 2; i++) {
            var s = new ActionDescriptor(); s.putObject(cTID("Clr "), cTID("RGBC"), rgbDesc(cs[i]));
            s.putEnumerated(cTID("Type"), cTID("Clry"), cTID("UsrS")); s.putInteger(cTID("Lctn"), loc[i]); s.putInteger(cTID("Mdpn"), 50);
            cl.putObject(cTID("Clrt"), s);
        }
        g.putList(cTID("Clrs"), cl);
        var tl = new ActionList();
        for (var k = 0; k < 2; k++) {
            var o = new ActionDescriptor(); o.putUnitDouble(cTID("Opct"), cTID("#Prc"), 100); o.putInteger(cTID("Lctn"), loc[k]); o.putInteger(cTID("Mdpn"), 50);
            tl.putObject(cTID("TrnS"), o);
        }
        g.putList(cTID("Trns"), tl);
        d.putObject(cTID("Grad"), cTID("Grdn"), g);
        executeAction(cTID("Grdn"), d, DialogModes.NO);
    }
    // real vector shape layer; kind = "Rctn" | "Elps"
    function shapeLayer(kind, name, x1, y1, x2, y2, c) {
        var d = new ActionDescriptor(), r = new ActionReference(); r.putClass(sTID("contentLayer")); d.putReference(cTID("null"), r);
        var ld = new ActionDescriptor(), cd = new ActionDescriptor(); cd.putObject(cTID("Clr "), cTID("RGBC"), rgbDesc([c.rgb.red, c.rgb.green, c.rgb.blue]));
        ld.putObject(cTID("Type"), sTID("solidColorLayer"), cd);
        var sd = new ActionDescriptor();
        sd.putUnitDouble(cTID("Top "), cTID("#Pxl"), y1); sd.putUnitDouble(cTID("Left"), cTID("#Pxl"), x1);
        sd.putUnitDouble(cTID("Btom"), cTID("#Pxl"), y2); sd.putUnitDouble(cTID("Rght"), cTID("#Pxl"), x2);
        ld.putObject(cTID("Shp "), cTID(kind), sd);
        d.putObject(cTID("Usng"), sTID("contentLayer"), ld);
        executeAction(cTID("Mk  "), d, DialogModes.NO);
        var l = app.activeDocument.activeLayer; l.name = name; return l;
    }
    function addMaskFromSelection() {
        var d = new ActionDescriptor(); d.putClass(cTID("Nw  "), cTID("Chnl"));
        var r = new ActionReference(); r.putEnumerated(cTID("Chnl"), cTID("Chnl"), cTID("Msk ")); d.putReference(cTID("At  "), r);
        d.putEnumerated(cTID("Usng"), cTID("UsrM"), cTID("RvlS")); executeAction(cTID("Mk  "), d, DialogModes.NO);
    }
    function hueSatLayer(hue, sat) {
        var d = new ActionDescriptor(), r = new ActionReference(); r.putClass(sTID("adjustmentLayer")); d.putReference(cTID("null"), r);
        var a = new ActionDescriptor(), t = new ActionDescriptor();
        t.putEnumerated(sTID("presetKind"), sTID("presetKindType"), sTID("presetKindCustom")); t.putBoolean(cTID("Clrz"), false);
        var l = new ActionList(), h = new ActionDescriptor();
        h.putEnumerated(cTID("Chnl"), cTID("Chnl"), cTID("Cmps")); h.putInteger(cTID("H   "), hue); h.putInteger(cTID("Strt"), sat); h.putInteger(cTID("Lght"), 0);
        l.putObject(cTID("Hst2"), h); t.putList(cTID("Adjs"), l);
        a.putObject(cTID("Type"), cTID("HStr"), t); d.putObject(cTID("Usng"), cTID("AdjL"), a);
        executeAction(cTID("Mk  "), d, DialogModes.NO);
    }
    function dropShadow() {
        var d = new ActionDescriptor(), r = new ActionReference();
        r.putProperty(cTID("Prpr"), cTID("Lefx")); r.putEnumerated(cTID("Lyr "), cTID("Ordn"), cTID("Trgt")); d.putReference(cTID("null"), r);
        var fx = new ActionDescriptor(); fx.putUnitDouble(cTID("Scl "), cTID("#Prc"), 100);
        var ds = new ActionDescriptor();
        ds.putBoolean(cTID("enab"), true); ds.putEnumerated(cTID("Md  "), cTID("BlnM"), cTID("Mltp"));
        ds.putObject(cTID("Clr "), cTID("RGBC"), rgbDesc([0, 0, 0]));
        ds.putUnitDouble(cTID("Opct"), cTID("#Prc"), 75); ds.putBoolean(cTID("uglg"), false);
        ds.putUnitDouble(cTID("lagl"), cTID("#Ang"), 120); ds.putUnitDouble(cTID("Dstn"), cTID("#Pxl"), 12);
        ds.putUnitDouble(cTID("Ckmt"), cTID("#Pxl"), 0); ds.putUnitDouble(cTID("blur"), cTID("#Pxl"), 12);
        ds.putUnitDouble(cTID("Nose"), cTID("#Prc"), 0); ds.putBoolean(cTID("AntA"), false);
        fx.putObject(cTID("DrSh"), cTID("DrSh"), ds); d.putObject(cTID("T   "), cTID("Lefx"), fx);
        executeAction(cTID("setd"), d, DialogModes.NO);
    }
    function addText(doc, str, x, y, size, c, name) {
        var l = doc.artLayers.add(); l.kind = LayerKind.TEXT; l.name = name || str;
        var t = l.textItem; t.contents = str; t.position = [x, y]; t.size = size; t.color = c;
        var f = findFont(); if (f) { try { t.font = f; } catch (e) { } }
        return l;
    }
    // run a build step; a failing step is logged and the build continues
    var stepWarn = [];
    function step(label, fn) { try { fn(); } catch (e) { stepWarn.push(label + ": " + e); log("  step failed [" + label + "]: " + e); } }

    // ---- document builders -------------------------------------------------------------------
    function newDoc(name, w, h, mode, bits, fill) {
        return app.documents.add(w, h, 72, name, mode, fill || DocumentFill.WHITE, 1.0, bits);
    }
    function fillBackground(doc, c) { selRect(doc, 0, 0, doc.width.as("px"), doc.height.as("px")); fillSel(doc, c); doc.selection.deselect(); }

    // the big "everything" document
    function buildRgb8Layers() {
        var W = 1200, H = 800, doc = newDoc("rgb8_layers", W, H, NewDocumentMode.RGB, BitsPerChannelType.EIGHT);
        step("background", function () { doc.activeLayer = doc.backgroundLayer; fillBackground(doc, rgb(240, 235, 224)); });
        step("gradient", function () {
            newLayer(doc, "Gradient band"); selRect(doc, 0, 0, W, 300); gradient([30, 60, 200], [220, 40, 160], 0, 0, W, 0); doc.selection.deselect();
        });
        step("blend multiply", function () {
            var l = newLayer(doc, "Multiply yellow"); selEllipse(300, 100, 700, 500); fillSel(doc, rgb(255, 220, 40)); doc.selection.deselect(); l.blendMode = BlendMode.MULTIPLY;
        });
        step("blend screen", function () {
            var l = newLayer(doc, "Screen cyan"); selEllipse(500, 150, 900, 550); fillSel(doc, rgb(40, 200, 220)); doc.selection.deselect(); l.blendMode = BlendMode.SCREEN;
        });
        step("blend overlay", function () {
            var l = newLayer(doc, "Overlay magenta"); selRect(doc, 800, 200, 1150, 520); fillSel(doc, rgb(220, 60, 180)); doc.selection.deselect(); l.blendMode = BlendMode.OVERLAY;
        });
        step("opacity 60", function () {
            var l = newLayer(doc, "Opacity 60"); selRect(doc, 100, 560, 600, 700); fillSel(doc, rgb(40, 170, 80)); doc.selection.deselect(); l.opacity = 60;
        });
        step("layer mask", function () {
            var l = newLayer(doc, "Masked orange"); selRect(doc, 650, 540, 1150, 770); fillSel(doc, rgb(240, 130, 20));
            selEllipse(680, 550, 1120, 760); addMaskFromSelection(); doc.selection.deselect();
        });
        step("shape rect + drop shadow", function () {
            shapeLayer("Rctn", "Shape rect", 60, 380, 400, 520, rgb(210, 40, 40)); dropShadow();
        });
        step("shape ellipse", function () { shapeLayer("Elps", "Shape ellipse", 420, 380, 560, 520, rgb(30, 90, 200)); });
        step("text", function () { addText(doc, "abthumb PSD corpus", 60, 360, 80, rgb(20, 20, 60), "Title text"); });
        step("group", function () {
            var g = doc.layerSets.add(); g.name = "Group A";
            var a = newLayer(doc, "Group square"); selRect(doc, 1000, 340, 1100, 440); fillSel(doc, rgb(90, 20, 140)); doc.selection.deselect(); a.move(g, ElementPlacement.INSIDE);
            var b = newLayer(doc, "Group dot"); selEllipse(1060, 400, 1140, 480); fillSel(doc, rgb(250, 250, 250)); doc.selection.deselect(); b.move(g, ElementPlacement.INSIDE);
        });
        step("hue/sat adjustment", function () { doc.activeLayer = doc.layers[0]; hueSatLayer(25, 20); });
        return doc;
    }

    // simpler layered scene, relative to W,H, for any mode / bit depth
    function buildSimple(name, W, H, mode, bits) {
        var doc = newDoc(name, W, H, mode, bits);
        step("background", function () { doc.activeLayer = doc.backgroundLayer; fillBackground(doc, rgb(240, 235, 224)); });
        step("gradient", function () {
            newLayer(doc, "Gradient band"); selRect(doc, 0, 0, W, Math.round(H * 0.35)); gradient([30, 60, 200], [220, 40, 160], 0, 0, W, 0); doc.selection.deselect();
        });
        step("multiply", function () {
            var l = newLayer(doc, "Multiply yellow");
            selEllipse(Math.round(W * 0.3), Math.round(H * 0.12), Math.round(W * 0.7), Math.round(H * 0.75)); fillSel(doc, rgb(255, 220, 40)); doc.selection.deselect();
            l.blendMode = BlendMode.MULTIPLY;
        });
        step("shape", function () {
            shapeLayer("Rctn", "Shape rect", Math.round(W * 0.05), Math.round(H * 0.5), Math.round(W * 0.3), Math.round(H * 0.85), rgb(210, 40, 40));
        });
        step("text", function () { addText(doc, "abthumb " + name, Math.round(W * 0.05), Math.round(H * 0.45), Math.max(12, Math.round(Math.min(W / 14, H / 6))), rgb(20, 20, 60), "Title text"); });
        return doc;
    }

    var BASES = [];
    function defBase(name, build, opts) { BASES.push({ name: name, build: build, o: opts || {} }); }

    defBase("rgb8_layers", buildRgb8Layers, { psb: true, eps: true, pdf: true });
    defBase("rgb16", function () { return buildSimple("rgb16", 1200, 800, NewDocumentMode.RGB, BitsPerChannelType.SIXTEEN); });
    defBase("rgb32", function () { return buildSimple("rgb32", 800, 600, NewDocumentMode.RGB, BitsPerChannelType.THIRTYTWO); });
    defBase("cmyk8", function () { return buildSimple("cmyk8", 1000, 800, NewDocumentMode.CMYK, BitsPerChannelType.EIGHT); }, { eps: true, noicc: true });
    defBase("gray8", function () { return buildSimple("gray8", 1000, 800, NewDocumentMode.GRAYSCALE, BitsPerChannelType.EIGHT); });
    defBase("lab8", function () { return buildSimple("lab8", 1000, 800, NewDocumentMode.LAB, BitsPerChannelType.EIGHT); });
    defBase("indexed", function () {
        var d = buildSimple("indexed", 800, 600, NewDocumentMode.RGB, BitsPerChannelType.EIGHT);
        d.flatten();
        var o = new IndexedConversionOptions(); o.palette = Palette.LOCALADAPTIVE; o.colors = 64; o.dither = Dither.DIFFUSION; o.ditherAmount = 75;
        d.changeMode(ChangeMode.INDEXEDCOLOR, o);
        return d;
    });
    defBase("bitmap", function () {
        var d = buildSimple("bitmap", 800, 600, NewDocumentMode.RGB, BitsPerChannelType.EIGHT);
        d.flatten(); d.changeMode(ChangeMode.GRAYSCALE);
        var o = new BitmapConversionOptions(); o.method = BitmapConversionType.DIFFUSIONDITHER; o.resolution = 72;
        d.changeMode(ChangeMode.BITMAP, o);
        return d;
    });
    defBase("transparent", function () {
        var W = 800, H = 600, doc = newDoc("transparent", W, H, NewDocumentMode.RGB, BitsPerChannelType.EIGHT, DocumentFill.TRANSPARENT);
        step("circle", function () {
            doc.activeLayer = doc.layers[0]; doc.activeLayer.name = "Only layer";
            selEllipse(100, 80, 460, 440); fillSel(doc, rgb(230, 60, 40)); doc.selection.deselect();
            selEllipse(380, 200, 720, 540); fillSel(doc, rgb(40, 110, 230)); doc.selection.deselect();
        });
        return doc;
    });
    defBase("wide", function () { return buildSimple("wide", 6000, 300, NewDocumentMode.RGB, BitsPerChannelType.EIGHT); });
    defBase("psb_big", function () {
        var W = 32000, H = 20000, doc = newDoc("psb_big", W, H, NewDocumentMode.RGB, BitsPerChannelType.EIGHT);
        // vertical-only gradient on the Background (compresses well with RLE), a few big vector shapes on top
        step("gradient", function () { doc.activeLayer = doc.backgroundLayer; gradient([20, 40, 120], [250, 220, 120], 0, 0, 0, H); });
        step("shape 1", function () { shapeLayer("Rctn", "Big rect", 2000, 2000, 14000, 9000, rgb(220, 50, 40)); });
        step("shape 2", function () { shapeLayer("Elps", "Big ellipse", 12000, 6000, 30000, 18000, rgb(40, 160, 90)); });
        step("text", function () { addText(doc, "psb_big 32000x20000", 1500, 17000, 1800, rgb(255, 255, 255), "Title text"); });
        return doc;
    }, { psb: true, psdToo: false, big: true });

    // ---- save helpers ------------------------------------------------------------------------
    function fileOK(f) { return f.exists && f.length > 0; }
    function doSave(rel, base, variant, options, w, h, prev, fn) {
        var f = new File(GEN + "/" + rel);
        try {
            if (f.exists) f.remove();
            fn(f);
            if (!fileOK(f)) throw new Error("file not written");
            row(rel, base, variant, options + (stepWarn.length ? " [build warnings: " + stepWarn.join(" | ") + "]" : ""), w, h, prev, true, "");
        } catch (e) {
            row(rel, base, variant, options, w, h, prev, false, e);
        }
    }
    function savePSDas(doc, f, embedICC) {
        var o = new PhotoshopSaveOptions(); o.layers = true; o.alphaChannels = true; o.embedColorProfile = embedICC; o.annotations = true; o.spotColors = true;
        doc.saveAs(f, o, true, Extension.LOWERCASE);
    }
    function savePSBas(f, compat) {
        var d = new ActionDescriptor(), o = new ActionDescriptor();
        o.putBoolean(sTID("maximizeCompatibility"), compat);
        d.putObject(cTID("As  "), sTID("largeDocumentFormat"), o);
        d.putPath(cTID("In  "), f); d.putBoolean(cTID("Cpy "), true); d.putBoolean(cTID("LwCs"), true);
        executeAction(cTID("save"), d, DialogModes.NO);
    }

    function saveVariants(doc, B, W, H) {
        var base = B.name;
        if (B.o.psdToo !== false) {
            var states = [["on", true], ["off", false]];
            for (var i = 0; i < states.length; i++) {
                (function (tag, on) {
                    setMaxCompat(on);
                    doSave("psd/" + base + "__ps_compat-" + tag + ".psd", base, "psd_compat_" + tag, "maximizeCompatibility=" + (on ? "ALWAYS" : "NEVER"), W, H, on ? "yes" : "unknown",
                        function (f) { savePSDas(doc, f, true); });
                    if (B.o.noicc) {
                        doSave("psd/" + base + "__ps_compat-" + tag + "_noicc.psd", base, "psd_compat_" + tag + "_noicc", "maximizeCompatibility=" + (on ? "ALWAYS" : "NEVER") + ",embedColorProfile=false", W, H, on ? "yes" : "unknown",
                            function (f) { savePSDas(doc, f, false); });
                    }
                })(states[i][0], states[i][1]);
            }
        }
        if (B.o.psb) {
            var st2 = [["on", true], ["off", false]];
            for (var k = 0; k < st2.length; k++) {
                (function (tag, on) {
                    setMaxCompat(on);
                    doSave("psb/" + base + "__ps_compat-" + tag + ".psb", base, "psb_compat_" + tag, "largeDocumentFormat,maximizeCompatibility=" + (on ? "true" : "false") + " (pref " + (on ? "ALWAYS" : "NEVER") + ")", W, H, on ? "yes" : "unknown",
                        function (f) { savePSBas(f, on); });
                })(st2[k][0], st2[k][1]);
            }
        }
        if (B.o.eps) {
            var ev = [["tiff8", Preview.EIGHTBITTIFF, "yes", "preview=EIGHTBITTIFF"], ["nopreview", Preview.NONE, "no", "preview=NONE"]];
            for (var e = 0; e < ev.length; e++) {
                (function (tag, pv, pe, desc) {
                    doSave("eps/" + base + "__ps_eps_" + tag + ".eps", base, "ps_eps_" + tag, "flattened copy," + desc + ",encoding=BINARY", W, H, pe, function (f) {
                        var dup = doc.duplicate(base + "_eps"); try { dup.flatten(); var o = new EPSSaveOptions(); o.preview = pv; o.encoding = SaveEncoding.BINARY; o.embedColorProfile = false; dup.saveAs(f, o, true, Extension.LOWERCASE); } finally { dup.close(SaveOptions.DONOTSAVECHANGES); }
                    });
                })(ev[e][0], ev[e][1], ev[e][2], ev[e][3]);
            }
        }
        if (B.o.pdf) {
            doSave("pdf/" + base + "__ps_pdf_layers.pdf", base, "ps_pdf_layers", "Photoshop PDF layers=true,preserveEditing=true", W, H, "yes", function (f) {
                var o = new PDFSaveOptions(); o.layers = true; o.preserveEditing = true; o.embedThumbnail = true; o.encoding = PDFEncoding.JPEG; o.jpegQuality = 10; o.view = false;
                doc.saveAs(f, o, true, Extension.LOWERCASE);
            });
            doSave("pdf/" + base + "__ps_pdf_flat.pdf", base, "ps_pdf_flat", "Photoshop PDF flattened copy,preserveEditing=false", W, H, "yes", function (f) {
                var dup = doc.duplicate(base + "_pdf");
                try { dup.flatten(); var o = new PDFSaveOptions(); o.layers = false; o.preserveEditing = false; o.embedThumbnail = false; o.encoding = PDFEncoding.JPEG; o.jpegQuality = 10; o.view = false; dup.saveAs(f, o, true, Extension.LOWERCASE); } finally { dup.close(SaveOptions.DONOTSAVECHANGES); }
            });
        }
    }

    // reference PNGs: duplicate -> flatten -> sRGB 8-bit -> long edge 1024 (and 256), bicubic
    function toSRGB8(d, keepAlpha) {
        if (!keepAlpha) d.flatten();
        if (d.mode == DocumentMode.BITMAP) d.changeMode(ChangeMode.GRAYSCALE);
        if (d.mode != DocumentMode.RGB) d.changeMode(ChangeMode.RGB);
        if (d.bitsPerChannel != BitsPerChannelType.EIGHT) d.bitsPerChannel = BitsPerChannelType.EIGHT;
        try { d.convertProfile("sRGB IEC61966-2.1", Intent.RELATIVECOLORIMETRIC, true, false); } catch (e) { log("  convertProfile failed: " + e); }
    }
    function savePNG(d, f) { var o = new PNGSaveOptions(); o.interlaced = false; d.saveAs(f, o, true, Extension.LOWERCASE); }
    function makeRefs(doc, B, W, H) {
        var base = B.name, d = null, keepAlpha = (base == "transparent");
        try {
            d = B.o.big ? doc : doc.duplicate(base + "_ref");   // big doc: work destructively on the original (closed afterwards)
            if (B.o.big) { d.flatten(); }
            toSRGB8(d, keepAlpha);
            var sizes = [1024, 256], lng = Math.max(W, H);
            for (var i = 0; i < sizes.length; i++) {
                (function (px) {
                    var ow = Math.max(1, Math.round(W * px / lng)), oh = Math.max(1, Math.round(H * px / lng));
                    doSave("ref/" + base + "__ps_" + px + ".png", base, "ref_png_" + px, "dup->flatten->sRGB 8-bit->resize bicubic" + (keepAlpha ? " (alpha kept)" : ""), ow, oh, "yes", function (f) {
                        d.resizeImage(UnitValue(ow, "px"), UnitValue(oh, "px"), null, ResampleMethod.BICUBIC);
                        savePNG(d, f);
                    });
                })(sizes[i]);
            }
        } catch (e) {
            row("ref/" + base + "__ps_*.png", base, "ref_png", "", "", "", "yes", false, e);
        }
        if (d && !B.o.big) { try { d.close(SaveOptions.DONOTSAVECHANGES); } catch (e) { } }
    }

    // ---- main loop ---------------------------------------------------------------------------
    try {
        for (var bi = 0; bi < BASES.length; bi++) {
            var B = BASES[bi];
            if (FILTER && FILTER.indexOf("," + B.name + ",") < 0) continue;
            var doc = null; stepWarn = [];
            log("=== building " + B.name);
            try {
                doc = B.build();
                var W = doc.width.as("px"), H = doc.height.as("px");
                log("  built " + W + "x" + H + " layers=" + doc.layers.length + " mode=" + doc.mode + " bits=" + doc.bitsPerChannel);
                saveVariants(doc, B, W, H);
                makeRefs(doc, B, W, H);   // last: destructive for the big doc
            } catch (e) {
                row("(base " + B.name + ")", B.name, "build", "", "", "", "unknown", false, e);
            }
            try { if (doc) doc.close(SaveOptions.DONOTSAVECHANGES); } catch (e) { log("close failed: " + e); }
        }
    } finally {
        try { if (origMax !== null) app.preferences.maximizeCompatibility = origMax; } catch (e) { }
        try { app.preferences.rulerUnits = origRuler; app.preferences.typeUnits = origType; } catch (e) { }
        try { app.displayDialogs = origDlg; } catch (e) { }
        manifest.close(); logf.writeln("done (maximizeCompatibility restored to " + origMax + ")"); logf.close();
    }
    return "gen_ps done";
})();
