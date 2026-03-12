/*
 * Copyright (C) 2008 Ronald Lamprecht
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *  
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#include "others/Wire.hh"
#include "errors.hh"
#include "main.hh"
#include "world.hh"

namespace enigma {
    namespace {
        bool capture_wire_anchor_ref(Stone *stone, Object::MpObjectRef &ref) {
            if (!stone)
                return false;
            const GridPos pos = stone->getOwnerPos();
            if (GetStone(pos) != stone)
                return false;
            ref = Object::MpObjectRef();
            ref.kind = Object::MpObjectRef::GRID_STONE;
            ref.pos = pos;
            return true;
        }

        Stone *resolve_wire_anchor_ref(const Object::MpObjectRef &ref) {
            if (ref.kind != Object::MpObjectRef::GRID_STONE)
                return NULL;
            return GetStone(ref.pos);
        }
    }  // namespace

    Wire::Wire() : Other(), anchor1 (NULL), anchor2 (NULL), model (NULL) {
    }
    
    std::string Wire::getClass() const {
        return "ot_wire";
    }

    void Wire::setAttr(const std::string &key, const Value &val) {
        if (key == "anchor1") {
            Stone *old = anchor1;
            anchor1 = dynamic_cast<Stone *>((Object *)val);
            ASSERT(anchor1 != NULL, XLevelRuntime, "Wire: 'anchor1' is no stone");
            ASSERT(anchor1 != anchor2, XLevelRuntime, "Wire: 'anchor1' is identical to 'anchor2'");
            switchAnchor(old, anchor1, anchor2);
        } else if (key == "anchor2") {
            Stone * old = anchor2;
            anchor2 = dynamic_cast<Stone *>((Object *)val);
            ASSERT(anchor2 != NULL, XLevelRuntime, "Wire: 'anchor2' is no stone");
            ASSERT(anchor2 != anchor1, XLevelRuntime, "Wire: 'anchor1' is identical to 'anchor2'");
            switchAnchor(old, anchor2, anchor1);
        }
        Other::setAttr(key, val);
    }
    
    Value Wire::getAttr(const std::string &key) const {
        if (key == "anchor1") {
            return anchor1;
        } else if (key == "anchor2") {
            return anchor2;
        }
        return Other::getAttr(key);
    }
    
    Value Wire::message(const Message &m) {
        if (m.message == "_performaction") {
            performAction(true);
            return Value();
        }
        return Other::message(m);
    }

    void Wire::MpCaptureAttrsForSnapshot(MpAttrSnapshot &attrs) const {
        Other::MpCaptureAttrsForSnapshot(attrs);
        attrs.emplace_back("anchor1", Value(anchor1));
        attrs.emplace_back("anchor2", Value(anchor2));
    }

    void Wire::MpRestoreAttrsForSnapshot(const MpAttrSnapshot &attrs) {
        MpAttrSnapshot remaining;
        remaining.reserve(attrs.size());

        Value anchor1_value;
        bool have_anchor1 = false;
        Value anchor2_value;
        bool have_anchor2 = false;

        for (const auto &entry : attrs) {
            if (entry.first == "anchor1") {
                anchor1_value = entry.second;
                have_anchor1 = true;
            } else if (entry.first == "anchor2") {
                anchor2_value = entry.second;
                have_anchor2 = true;
            } else {
                remaining.push_back(entry);
            }
        }

        Other::MpRestoreAttrsForSnapshot(remaining);

        if (anchor1 != NULL || anchor2 != NULL) {
            switchAnchor(anchor1, NULL, anchor2);
            switchAnchor(anchor2, NULL, anchor1);
            anchor1 = NULL;
            anchor2 = NULL;
        }

        if (have_anchor1)
            setAttr("anchor1", anchor1_value);
        if (have_anchor2)
            setAttr("anchor2", anchor2_value);
    }

    void Wire::MpCaptureSemanticState(MpSemanticState &semantic) const {
        semantic.logical_state = MpCaptureStateForSnapshot();
        semantic.flags = MpCaptureFlagsForSnapshot();
        semantic.fields.clear();
        semantic.refs.clear();

        Object::MpObjectRef ref;
        if (capture_wire_anchor_ref(anchor1, ref))
            semantic.refs.emplace_back("anchor1", ref);
        if (capture_wire_anchor_ref(anchor2, ref))
            semantic.refs.emplace_back("anchor2", ref);
    }

    bool Wire::MpApplySemanticState(const MpSemanticState &semantic, MpApplyContext ctx) {
        (void)ctx;
        Stone *resolved_anchor1 = NULL;
        Stone *resolved_anchor2 = NULL;
        bool have_anchor1 = false;
        bool have_anchor2 = false;
        for (const auto &entry : semantic.refs) {
            if (entry.first == "anchor1") {
                resolved_anchor1 = resolve_wire_anchor_ref(entry.second);
                have_anchor1 = resolved_anchor1 != NULL;
            } else if (entry.first == "anchor2") {
                resolved_anchor2 = resolve_wire_anchor_ref(entry.second);
                have_anchor2 = resolved_anchor2 != NULL;
            }
        }
        if (!have_anchor1 || !have_anchor2)
            return false;

        MpAttrSnapshot attrs;
        attrs.emplace_back("anchor1", Value(static_cast<Object *>(resolved_anchor1)));
        attrs.emplace_back("anchor2", Value(static_cast<Object *>(resolved_anchor2)));
        MpRestoreFlagsForSnapshot(semantic.flags);
        MpRestoreAttrsForSnapshot(attrs);
        return true;
    }

    bool Wire::MpNeedsSemanticWorldResync() const {
        return true;
    }

    void Wire::postAddition() {
//        model = display::AddRubber(anchor1->getOwnerPos().center(), anchor2->getOwnerPos().center(), 100, 255, 30, true);    // lime
        model = display::AddRubber(anchor1->getOwnerPos().center(), anchor2->getOwnerPos().center(), 200, 50, 150, true);    // purple
    }
    
    void Wire::preRemoval() {
        model.kill();
        switchAnchor(anchor1, NULL, anchor2);
        switchAnchor(anchor2, NULL, anchor1);        
    }
    
    void Wire::tick(double dt) {  // TODO maybe we should let the stones inform the wires on every move
        model.update_first(anchor1->getOwnerPos().center());
        model.update_second(anchor2->getOwnerPos().center());
    }
    
    
    void Wire::switchAnchor(Object *oldAnchor, Object *newAnchor, Object *otherAnchor) {
        if (oldAnchor != NULL) {
            ObjectList olist = oldAnchor->getAttr("wires");
            olist.remove(this);
            oldAnchor->setAttr("wires", olist);
            if (otherAnchor != NULL) {
                // remove both anchors from each others fellows list
                olist = oldAnchor->getAttr("fellows");
                ObjectList::iterator it = find(olist.begin(), olist.end(), otherAnchor);
                if (it != olist.end()) {
                    olist.erase(it);
                }
                oldAnchor->setAttr("fellows", olist);
                olist = otherAnchor->getAttr("fellows");
                it = find(olist.begin(), olist.end(), oldAnchor);
                if (it != olist.end()) {
                    olist.erase(it);
                }
                otherAnchor->setAttr("fellows", olist);
            }            
        }
        if (newAnchor != NULL) {
            ObjectList olist = newAnchor->getAttr("wires");
            olist.push_back(this);
            newAnchor->setAttr("wires", olist);
            if (otherAnchor != NULL) {
                // add both anchors to each others fellows list
                olist = newAnchor->getAttr("fellows");
                ObjectList::iterator it = find(olist.begin(), olist.end(), otherAnchor);
                if (it != olist.end()) {
                    // we are a replacement wire for an already existing wire - remove it
                    olist = newAnchor->getAttr("wires");
                    for (ObjectList::iterator itr = olist.begin(); itr != olist.end(); ++itr) {
                        Wire *w = dynamic_cast<Wire *>(*itr);
                        if (w != NULL && (((Object *)(w->getAttr("anchor1")) == newAnchor &&  (Object *)(w->getAttr("anchor2")) == otherAnchor)
                                || ((Object *)(w->getAttr("anchor2")) == newAnchor &&  (Object *)(w->getAttr("anchor1")) == otherAnchor)))
                            KillOther(w);
                            break;
                    }
                    // reload fellows
                    olist = newAnchor->getAttr("fellows");
                }
                olist.push_back(otherAnchor);
                newAnchor->setAttr("fellows", olist);
                olist = otherAnchor->getAttr("fellows");
                olist.push_back(newAnchor);
                otherAnchor->setAttr("fellows", olist);
            }
        }
    }

    BOOT_REGISTER_START
        BootRegister(new Wire(), "ot_wire");
    BOOT_REGISTER_END

} // namespace enigma
