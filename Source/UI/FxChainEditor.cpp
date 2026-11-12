#include "FxChainEditor.h"
#include "Core/Paths.h"
#include "Engine/Catalog.h"

namespace UI {

juce::File fxPresetDir(const juce::String& type) { auto d = Paths::presets().getChildFile("Effects").getChildFile(type); d.createDirectory(); return d; }
juce::File chainPresetDir() { auto d = Paths::presets().getChildFile("Chains"); d.createDirectory(); return d; }

juce::String effectDisplayName(const EffectData& e) {
    if (e.name.isNotEmpty()) return e.name;
    const auto* info = Catalog::findEffect(e.type);
    return info ? juce::String(info->name) : e.type;
}

bool applyChainPreset(Project& p, std::vector<EffectData>& chain, const juce::String& name) {
    for (auto& c : Catalog::chainPresets())
        if (name == c.name) {
            chain.clear();
            for (auto& [type, values] : c.effects) { auto fx = p.makeEffect(type); Catalog::applyValues(*fx.params, values); chain.push_back(fx); }
            return true;
        }
    const auto f = chainPresetDir().getChildFile(juce::File::createLegalFileName(name) + ".json");
    if (!f.existsAsFile()) return false;
    const auto v = juce::JSON::parse(f);
    if (!v.isArray()) return false;
    chain.clear();
    for (auto& e : *v.getArray()) {
        const auto type = e["type"].toString();
        if (!Catalog::findEffect(type)) continue;
        auto fx = p.makeEffect(type);
        fx.name = e["name"].toString();
        fx.params->fromVar(e["params"]);
        fx.bypass->store((bool)e["bypass"]);
        chain.push_back(fx);
    }
    return true;
}

// ---------------------------------------------------------------- list
class FxChainEditor::List : public juce::Component {
public:
    explicit List(FxChainEditor& o) : owner(o) {}
    static constexpr int rowH = 56;
    std::function<void(const juce::String&)> onSelect;
    std::function<void(int, juce::Component*)> onMenu;
    void paint(juce::Graphics& g) override {
        const auto& p = pal();
        auto* v = owner.get();
        if (!v) return;
        for (int i = 0; i < (int)v->size(); ++i) {
            auto& e = (*v)[(size_t)i];
            auto r = juce::Rectangle<int>(0, i * rowH, getWidth(), rowH).reduced(2).toFloat();
            const bool sel = e.id == owner.selectedFx;
            const bool byp = e.bypass->load();
            g.setColour(sel ? p.raised : p.surface);
            g.fillRoundedRectangle(r, rControl);
            if (sel) { g.setColour(p.accent); g.fillRoundedRectangle(r.withWidth(4.f), 2.f); }
            auto pw = r.removeFromLeft(48.f);
            drawIcon(g, "power", pw.withSizeKeepingCentre(22.f, 22.f), byp ? p.faint : p.play, 2.f);
            auto more = r.removeFromRight(44.f);
            drawIcon(g, "more", more.withSizeKeepingCentre(20.f, 20.f), p.dim);
            const auto* info = Catalog::findEffect(e.type);
            g.setColour(byp ? p.faint : p.text);
            g.setFont(font(15.f, true));
            g.drawFittedText(effectDisplayName(e), r.removeFromTop(r.getHeight() * 0.55f).toNearestInt(), juce::Justification::bottomLeft, 1);
            g.setColour(p.dim);
            g.setFont(font(12.f));
            const juce::String sub = byp ? juce::String("Bypassed") : (e.name.isNotEmpty() && info ? juce::String(info->name) : juce::String(info ? info->category : ""));
            g.drawFittedText(sub, r.toNearestInt(), juce::Justification::topLeft, 1);
        }
        auto add = juce::Rectangle<int>(0, (int)v->size() * rowH, getWidth(), rowH).reduced(2).toFloat();
        g.setColour(p.line);
        g.drawRoundedRectangle(add.reduced(1.f), rControl, 1.5f);
        drawIcon(g, "plus", add.removeFromLeft(48.f).withSizeKeepingCentre(20.f, 20.f), p.text);
        g.setColour(p.text);
        g.setFont(font(15.f, true));
        g.drawText("Add effect", add.toNearestInt(), juce::Justification::centredLeft);
    }
    void mouseUp(const juce::MouseEvent& e) override {
        auto* v = owner.get();
        if (!v || e.getDistanceFromDragStart() > 8) return;
        const int i = e.y / rowH;
        if (i == (int)v->size()) { if (onMenu) onMenu(-1, this); return; }
        if (i < 0 || i > (int)v->size()) return;
        auto& fx = (*v)[(size_t)i];
        auto& s = *ctx().session;
        if (e.x < 50) { s.checkpoint("Bypass"); fx.bypass->store(!fx.bypass->load()); s.changed(); haptic(0); repaint(); return; }
        if (e.x > getWidth() - 46 || e.mods.isPopupMenu()) { if (onMenu) onMenu(i, this); return; }
        if (onSelect) onSelect(fx.id);
    }
private:
    FxChainEditor& owner;
};

// ---------------------------------------------------------------- editor
FxChainEditor::FxChainEditor() : list(std::make_unique<List>(*this)) {
    addAndMakeVisible(listView);
    listView.setViewedComponent(list.get(), false);
    listView.setScrollBarsShown(true, false);
    listView.setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::nonHover);
    list->onSelect = [this](const juce::String& id) { selectedFx = id; refresh(); };
    list->onMenu = [this](int i, juce::Component* c) { if (i < 0) addMenu(c); else fxMenu(i, c); };
    addAndMakeVisible(editView);
    editView.setViewedComponent(&panel, false);
    editView.setScrollBarsShown(true, false);
    editView.setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::nonHover);
    panel.onChange = [this] {
        // Effect Designer: picking a new module type starts its knobs at useful settings
        if (auto* fx = selected(); fx && fx->type == "designer") {
            for (int s = 0; s < 6; ++s) {
                const int ti = fx->params->indexOf("s" + juce::String(s + 1) + "_type");
                const int type = fx->params->getInt(ti);
                if (type != designerTypes[(size_t)s]) {
                    if (designerTypes[(size_t)s] >= 0) {
                        float d[4];
                        Catalog::designerDefaults(type, d);
                        int k = 0;
                        for (auto suffix : { "_a", "_b", "_c", "_mix" }) fx->params->set(fx->params->indexOf("s" + juce::String(s + 1) + suffix), d[k++]);
                    }
                    designerTypes[(size_t)s] = type;
                }
            }
        }
        panel.repaint();
    };
    addAndMakeVisible(presetB);
    presetB.onClick = [this] { presetMenu(); };
    addAndMakeVisible(chainB);
    chainB.onClick = [this] { showChainMenu(&chainB); };
    startTimerHz(15);
}
FxChainEditor::~FxChainEditor() { listView.setViewedComponent(nullptr, false); }

EffectData* FxChainEditor::selected() {
    if (auto* v = get()) for (auto& e : *v) if (e.id == selectedFx) return &e;
    return nullptr;
}

void FxChainEditor::configurePanel(const EffectData* fx) {
    panel.knobLabel = nullptr;
    panel.knobFormat = nullptr;
    if (fx && fx->type == "designer") {
        // module knobs say what they do for the chosen module type
        auto ps = fx->params;
        auto parse = [ps](int i, int& slot, int& knob) {
            const juce::String id(ps->meta(i).id);
            if (id.length() != 4 || id[0] != 's' || id[2] != '_') return false;
            slot = id[1] - '1';
            knob = id[3] == 'a' ? 0 : (id[3] == 'b' ? 1 : (id[3] == 'c' ? 2 : -1));
            return knob >= 0;
        };
        panel.knobLabel = [ps, parse](int i) {
            int slot, knob;
            if (!parse(i, slot, knob)) return juce::String(ps->meta(i).name);
            const int type = ps->getInt(ps->indexOf("s" + juce::String(slot + 1) + "_type"));
            return juce::String(Catalog::designerKnobName(type, knob));
        };
        panel.knobFormat = [ps, parse](int i, float v) {
            int slot, knob;
            if (!parse(i, slot, knob)) return ps->format(i, v);
            const int type = ps->getInt(ps->indexOf("s" + juce::String(slot + 1) + "_type"));
            return Catalog::designerFormat(type, knob, v * 0.01f);
        };
    }
    juce::Colour col;
    if (auto* t = ctx().session->project.findTrack(trackId)) col = t->colour;
    panel.setParams(fx ? fx->params : nullptr, fx && automationPrefix.isNotEmpty() ? automationPrefix + fx->id + ":" : juce::String(), trackId, col);
}

void FxChainEditor::refresh() {
    auto* v = get();
    if (!v) { configurePanel(nullptr); lastSig.clear(); repaint(); return; }
    if (!selected()) selectedFx = v->empty() ? juce::String() : v->front().id;
    const EffectData* fx = selected();
    const auto sig = fx ? fx->id + juce::String((juce::pointer_sized_int)fx->params.get()) : juce::String();
    if (sig != lastSig) {
        lastSig = sig;
        configurePanel(fx);
        for (int s = 0; s < 6; ++s) designerTypes[(size_t)s] = fx && fx->type == "designer" ? fx->params->getInt(fx->params->indexOf("s" + juce::String(s + 1) + "_type")) : -1;
    }
    presetB.setVisible(fx != nullptr);
    resized();
    list->repaint();
    repaint();
}

void FxChainEditor::timerCallback() { if (isShowing() && !meterArea.isEmpty()) repaint(meterArea); }

void FxChainEditor::paint(juce::Graphics& g) {
    const EffectData* fx = const_cast<FxChainEditor*>(this)->selected();
    if (!fx) {
        g.setColour(pal().dim); g.setFont(font(15.f));
        g.drawFittedText(emptyText, editView.getBounds().reduced(16), juce::Justification::topLeft, 4);
        return;
    }
    const auto* info = Catalog::findEffect(fx->type);
    auto ta = titleArea;
    heading(g, effectDisplayName(*fx), ta.removeFromTop(26), 20.f);
    g.setColour(pal().dim); g.setFont(font(13.f));
    g.drawFittedText(info ? info->description : "", ta, juce::Justification::topLeft, 2);
    if (info && juce::String(info->category) == "Dynamics") {
        float gr = 0.f;
        if (auto* e = ctx().engine->effectFor(fx->id)) gr = e->meterDb.load();
        auto m = meterArea.toFloat();
        g.setColour(pal().raised); g.fillRoundedRectangle(m, 5.f);
        const float w = juce::jlimit(0.f, 1.f, -gr / 24.f) * m.getWidth();
        g.setColour(pal().warn); g.fillRoundedRectangle(m.withX(m.getRight() - w).withWidth(w), 5.f);
        g.setColour(pal().text); g.setFont(font(11.f, true));
        g.drawText("Gain reduction " + juce::String(-gr, 1) + " dB", m.toNearestInt().reduced(8, 0), juce::Justification::centredLeft);
    }
}

void FxChainEditor::resized() {
    auto r = getLocalBounds();
    narrow = r.getWidth() < 620;
    auto* v = get();
    const int n = v ? (int)v->size() + 1 : 1;
    if (narrow) {
        auto top = r.removeFromTop(44);
        chainB.setBounds(top.removeFromLeft(juce::jmin(200, top.getWidth())).reduced(0, 2));
        const int h = juce::jmin(n, 3) * List::rowH + 4;
        listView.setBounds(r.removeFromTop(h));
        r.removeFromTop(8);
    } else {
        auto left = r.removeFromLeft(juce::jmin(300, r.getWidth() / 3));
        chainB.setBounds(left.removeFromTop(44).reduced(0, 2));
        left.removeFromTop(6);
        listView.setBounds(left);
        r.removeFromLeft(14);
    }
    list->setSize(listView.getWidth() - 8, n * List::rowH);
    auto head = r.removeFromTop(66);
    presetB.setBounds(head.removeFromRight(130).withHeight(44));
    titleArea = head.withTrimmedRight(8);
    const auto* fx = selected();
    const auto* info = fx ? Catalog::findEffect(fx->type) : nullptr;
    meterArea = info && juce::String(info->category) == "Dynamics" ? r.removeFromTop(26).withTrimmedBottom(4) : juce::Rectangle<int>();
    r.removeFromTop(6);
    editView.setBounds(r);
    panel.setSize(juce::jmax(100, r.getWidth() - 10), panel.preferredHeight(juce::jmax(100, r.getWidth() - 10)) + 20);
}

void FxChainEditor::addMenu(juce::Component* c) {
    juce::PopupMenu m, mine;
    std::map<juce::String, juce::PopupMenu> cats;
    int i = 0;
    for (auto& e : Catalog::effects()) cats[e.category].addItem(100 + i++, e.name);
    for (auto c2 : { "Dynamics", "EQ", "Time", "Distortion", "Other" }) m.addSubMenu(c2, cats[c2]);
    // saved effects of every type ("My effects"): adding one brings its settings and name
    std::vector<std::pair<juce::String, juce::File>> saved;
    for (auto& e : Catalog::effects())
        for (auto& f : fxPresetDir(e.id).findChildFiles(juce::File::findFiles, false, "*.json")) {
            saved.push_back({ e.id, f });
            mine.addItem(1000 + (int)saved.size() - 1, f.getFileNameWithoutExtension() + "  (" + e.name + ")");
        }
    m.addSeparator();
    m.addSubMenu("My effects", mine, !saved.empty());
    m.addItem(50, "Design a new effect...");
    showMenu(m, c, [this, saved](int r) {
        auto* v = get();
        if (!v || r <= 0) return;
        auto& s = *ctx().session;
        EffectData fx;
        if (r == 50) {
            fx = s.project.makeEffect("designer");
            ctx().toast("Pick a module for each slot; knobs relabel to match");
        } else if (r >= 1000) {
            const auto& [type, file] = saved[(size_t)r - 1000];
            fx = s.project.makeEffect(type);
            fx.params->fromVar(juce::JSON::parse(file));
            fx.name = file.getFileNameWithoutExtension();
        } else {
            const auto& info = Catalog::effects()[(size_t)r - 100];
            fx = s.project.makeEffect(info.id);
            const juce::String id(info.id);
            if (returnTrack && (id == "reverb" || id.contains("delay") || id == "pingpong" || id == "echo")) Catalog::applyValues(*fx.params, "mix=100");
        }
        s.checkpoint("Add " + effectDisplayName(fx));
        v = get();
        selectedFx = fx.id;
        v->push_back(fx);
        s.changed(true);
        refresh();
    });
}

void FxChainEditor::fxMenu(int i, juce::Component* c) {
    auto* v = get();
    if (!v || i >= (int)v->size()) return;
    juce::PopupMenu m;
    m.addItem(1, "Edit");
    m.addItem(2, (*v)[(size_t)i].bypass->load() ? "Turn on" : "Bypass");
    m.addItem(3, "Move up", i > 0);
    m.addItem(4, "Move down", i < (int)v->size() - 1);
    m.addItem(5, "Duplicate");
    m.addItem(7, "Rename...");
    m.addItem(8, "Save to My effects...");
    m.addSeparator();
    m.addItem(6, "Remove");
    showMenu(m, c, [this, i](int r) {
        auto* vv = get();
        auto& s = *ctx().session;
        if (!vv || i >= (int)vv->size() || r == 0) return;
        auto& e = (*vv)[(size_t)i];
        if (r == 1) { selectedFx = e.id; refresh(); return; }
        if (r == 7) {
            askText("Rename effect", "Shown in the chain instead of the effect's type.", effectDisplayName(e), "Rename", [this, id = e.id](const juce::String& n) {
                for (auto& x : *get()) if (x.id == id) { ctx().session->checkpoint("Rename effect"); x.name = n.trim(); ctx().session->changed(); }
                refresh();
            });
            return;
        }
        if (r == 8) {
            askText("Save to My effects", "Saved effects appear under \"My effects\" when you add an effect, on any track.", effectDisplayName(e), "Save",
                    [type = e.type, ps = e.params](const juce::String& n) {
                        if (n.trim().isEmpty()) return;
                        fxPresetDir(type).getChildFile(juce::File::createLegalFileName(n.trim()) + ".json").replaceWithText(juce::JSON::toString(ps->toVar()));
                        ctx().toast("Saved to My effects");
                    });
            return;
        }
        s.checkpoint("Edit effect chain");
        vv = get();
        auto& v2 = *vv;
        if (r == 2) v2[(size_t)i].bypass->store(!v2[(size_t)i].bypass->load());
        else if (r == 3) std::swap(v2[(size_t)i], v2[(size_t)i - 1]);
        else if (r == 4) std::swap(v2[(size_t)i], v2[(size_t)i + 1]);
        else if (r == 5) { auto d = s.project.makeEffect(v2[(size_t)i].type); d.params->copyFrom(*v2[(size_t)i].params); d.name = v2[(size_t)i].name; v2.insert(v2.begin() + i + 1, d); }
        else if (r == 6) {
            const auto id = v2[(size_t)i].id;
            if (auto* t = s.project.findTrack(trackId))
                t->automation.erase(std::remove_if(t->automation.begin(), t->automation.end(), [&id](auto& l) { return l.target.startsWith("fx:" + id); }), t->automation.end());
            v2.erase(v2.begin() + i);
        }
        s.changed(true);
        refresh();
    });
}

void FxChainEditor::presetMenu() {
    auto* fx = selected();
    if (!fx) return;
    juce::PopupMenu m, mine;
    std::vector<const EffectPreset*> factory;
    for (auto& p : Catalog::effectPresets()) if (fx->type == p.effect) { factory.push_back(&p); m.addItem((int)factory.size(), p.name); }
    juce::Array<juce::File> files;
    for (auto& f : fxPresetDir(fx->type).findChildFiles(juce::File::findFiles, false, "*.json")) { files.add(f); mine.addItem(500 + files.size() - 1, f.getFileNameWithoutExtension()); }
    m.addSubMenu("My presets", mine, files.size() > 0);
    m.addSeparator();
    m.addItem(900, "Reset to default");
    m.addItem(901, "Save as preset...");
    showMenu(m, &presetB, [this, factory, files, type = fx->type](int r) {
        auto* f = selected();
        if (!f || r == 0) return;
        if (r == 901) {
            askText("Save effect preset", "Also appears under \"My effects\" when you add an effect.", effectDisplayName(*f), "Save", [this, type](const juce::String& n) {
                if (auto* e = selected()) if (n.trim().isNotEmpty()) fxPresetDir(type).getChildFile(juce::File::createLegalFileName(n.trim()) + ".json").replaceWithText(juce::JSON::toString(e->params->toVar()));
                ctx().toast("Preset saved");
            });
            return;
        }
        ctx().session->checkpoint("Effect preset");
        f->params->resetToDefaults();
        if (r >= 1 && r <= (int)factory.size()) Catalog::applyValues(*f->params, factory[(size_t)r - 1]->values);
        else if (r >= 500 && r < 900) f->params->fromVar(juce::JSON::parse(files[r - 500]));
        if (f->type == "designer")   // the preset's own module settings stay as they are
            for (int k = 0; k < 6; ++k) designerTypes[(size_t)k] = f->params->getInt(f->params->indexOf("s" + juce::String(k + 1) + "_type"));
        ctx().session->changed();
        panel.repaint();
    });
}

void FxChainEditor::showChainMenu(juce::Component* target) {
    juce::PopupMenu m, vocal, inst, mine;
    std::vector<juce::String> names;
    for (auto& c : Catalog::chainPresets()) {
        names.push_back(c.name);
        (juce::String(c.category) == "Vocals" ? vocal : inst).addItem((int)names.size(), c.name);
    }
    juce::StringArray saved;
    for (auto& f : chainPresetDir().findChildFiles(juce::File::findFiles, false, "*.json")) { saved.add(f.getFileNameWithoutExtension()); mine.addItem(500 + saved.size() - 1, saved[saved.size() - 1]); }
    if (chainCategory == "Instruments") { m.addSubMenu("Instrument chains", inst); m.addSubMenu("Vocal chains", vocal); }
    else { m.addSubMenu("Vocal chains", vocal); m.addSubMenu("Instrument chains", inst); }
    m.addSubMenu("My chains", mine, saved.size() > 0);
    m.addSeparator();
    auto* v = get();
    m.addItem(900, "Save this chain...", v && !v->empty());
    m.addItem(901, "Remove all effects", v && !v->empty());
    showMenu(m, target, [this, names, saved](int r) {
        auto* vv = get();
        auto& s = *ctx().session;
        if (!vv || r == 0) return;
        if (r == 900) {
            askText("Save chain", "Saves every effect in this chain with its settings.", "My chain", "Save", [this](const juce::String& n) {
                auto* c = get();
                if (!c || n.trim().isEmpty()) return;
                juce::Array<juce::var> arr;
                for (auto& e : *c) {
                    auto* o = new juce::DynamicObject();
                    o->setProperty("type", e.type); o->setProperty("name", e.name);
                    o->setProperty("bypass", e.bypass->load()); o->setProperty("params", e.params->toVar());
                    arr.add(juce::var(o));
                }
                chainPresetDir().getChildFile(juce::File::createLegalFileName(n.trim()) + ".json").replaceWithText(juce::JSON::toString(juce::var(arr)));
                ctx().toast("Chain saved");
            });
            return;
        }
        const juce::String name = r == 901 ? juce::String() : (r >= 500 ? saved[r - 500] : names[(size_t)r - 1]);
        s.checkpoint(r == 901 ? "Remove all effects" : "Chain preset " + name);
        vv = get();
        if (r == 901) vv->clear();
        else applyChainPreset(s.project, *vv, name);
        selectedFx = vv->empty() ? juce::String() : vv->front().id;
        s.changed(true);
        refresh();
        if (r != 901) ctx().toast("Loaded \"" + name + "\" (undo to go back)");
    });
}

}  // namespace UI
