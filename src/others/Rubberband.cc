/*
 * Copyright (C) 2008,2009 Ronald Lamprecht
 * Copyright (C) 2010 Raoul Bourquin
 * 2026 LLM generated contribution - concept, review and revision by Ferdinand Strixner
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
#include "others/Rubberband.hh"
#include "actors.hh"
#include "errors.hh"
#include "main.hh"
#include "server.hh"
#include "world.hh"

namespace enigma {
    namespace {
        bool capture_rubber_anchor_ref(Object *obj, Object::MpObjectRef &ref) {
            if (!obj)
                return false;
            if (Actor *actor = dynamic_cast<Actor *>(obj)) {
                ref = Object::MpObjectRef();
                ref.kind = Object::MpObjectRef::ACTOR_STABLE_ID;
                ref.stable_id = actor->stable_id();
                return true;
            }
            if (Stone *stone = dynamic_cast<Stone *>(obj)) {
                const GridPos pos = stone->getOwnerPos();
                if (GetStone(pos) != stone)
                    return false;
                ref = Object::MpObjectRef();
                ref.kind = Object::MpObjectRef::GRID_STONE;
                ref.pos = pos;
                return true;
            }
            if (Value name = obj->getAttr("name")) {
                const std::string str = name.to_string();
                if (!str.empty()) {
                    ref = Object::MpObjectRef();
                    ref.kind = Object::MpObjectRef::OTHER_BY_NAME;
                    ref.name = str;
                    return true;
                }
            }
            return false;
        }

        Object *resolve_rubber_anchor_ref(const Object::MpObjectRef &ref) {
            switch (ref.kind) {
            case Object::MpObjectRef::ACTOR_OBJECT:
                return Object::getObject(static_cast<int>(ref.object_id));
            case Object::MpObjectRef::ACTOR_STABLE_ID:
                return FindActorByStableId(ref.stable_id);
            case Object::MpObjectRef::GRID_STONE:
                return GetStone(ref.pos);
            case Object::MpObjectRef::OTHER_BY_NAME:
                return GetNamedObject(ref.name);
            default:
                return NULL;
            }
        }
    }  // namespace

    Rubberband::Rubberband() : strength (10), outerThreshold (1), innerThreshold (0), minLength (0), maxLength (0) {
        anchor1 = NULL;
        anchor2.ac = NULL;
    }

    std::string Rubberband::getClass() const {
        return "ot_rubberband";
    }

    void Rubberband::setAttr(const std::string &key, const Value &val) {
        if (key == "anchor1") {
            Actor *old = anchor1;
            anchor1 = dynamic_cast<Actor *>((Object *)val);
            ASSERT(anchor1 != NULL, XLevelRuntime, "Rubberband: 'anchor1' is no actor");
            ASSERT(anchor1 != anchor2.ac, XLevelRuntime, "Rubberband: 'anchor1' is identical to 'anchor2'");
            switchAnchor(old, anchor1, anchor2Object());
        } else if (key == "anchor2") {
            Object * old = anchor2Object();
            Object * obj = val;
            if (obj != NULL && obj->getObjectType() == Object::ACTOR) {
                anchor2.ac = dynamic_cast<Actor *>((Object *)val);
                ASSERT(anchor1 != anchor2.ac, XLevelRuntime, "Rubberband: 'anchor1' is identical to 'anchor2'");
                objFlags &= ~OBJBIT_STONE;
                switchAnchor(old, anchor2.ac, anchor1);
            } else if (obj != NULL && obj->getObjectType() == Object::STONE) {
                anchor2.st = dynamic_cast<Stone *>((Object *)val);
                objFlags |= OBJBIT_STONE;
                switchAnchor(old, anchor2.st, anchor1);
            } else
                ASSERT(false, XLevelRuntime, "Rubberband: 'anchor2' is neither actor nor stone");
        } else if (key == "strength") {
            strength =  (val.getType() == Value::NIL) ? 10.0 : (double)val;
        } else if (key == "length") {
            outerThreshold = (val.getType() == Value::NIL) ? 1.0 : (double)val;
            ASSERT((outerThreshold >= 0) || (outerThreshold == -1.0), XLevelRuntime, "Rubberband: length is negative");
        } else if (key == "threshold") {
            innerThreshold = val;
            ASSERT(innerThreshold >= 0, XLevelRuntime, "Rubberband: inner threshold is negative");
        } else if (key == "max") {
            maxLength = val;
            ASSERT((maxLength >= 0) && (maxLength == 0 || maxLength >= minLength), XLevelRuntime, "Rubberband: max length is negative or less min");
        } else if (key == "min") {
            minLength = val;
            ASSERT((minLength >= 0) && (maxLength == 0 || maxLength >= minLength), XLevelRuntime, "Rubberband: min length is negative or greater max");
        }
        Other::setAttr(key, val);
    }

    Value Rubberband::getAttr(const std::string &key) const {
        if (key == "anchor1") {
            return anchor1;
        } else if (key == "anchor2") {
            return anchor2Object();
        } else if (key == "strength") {
            return strength;
        } else if (key == "length") {
            return outerThreshold;
        } else if (key == "threshold") {
            return innerThreshold;
        } else if (key == "max") {
            return maxLength;
        } else if (key == "min") {
            return minLength;
        }
        return Other::getAttr(key);
    }

    Value Rubberband::message(const Message &m) {
        if (m.message == "_recheck") {
            ecl::V2 v = posAnchor2() - anchor1->get_pos();
            double len = ecl::length(v);
            bool violating = false;
            if (maxLength > 0 && len > maxLength) {
                objFlags |= OBJBIT_MAXVIOLATION;
                violating = true;
            } else if (len < minLength) {
                objFlags |= OBJBIT_MINVIOLATION;
                violating = true;
            }
            if (violating) {
                performAction(false);
            }
            return Value();
        } else if (m.message == "_mp_resync_flags") {
            // Multiplayer resync helper: recompute max/min violation flags from
            // current anchor positions without triggering performAction().
            //
            // This keeps peers aligned when a soft resync teleports actors, and
            // avoids persistent divergence caused by stale OBJBIT_*VIOLATION flags.
            ecl::V2 v = posAnchor2() - anchor1->get_pos();
            double len = ecl::length(v);
            if ((objFlags & OBJBIT_MAXVIOLATION) && (maxLength > 0) && (len <= maxLength))
                objFlags &= ~OBJBIT_MAXVIOLATION;
            if ((objFlags & OBJBIT_MINVIOLATION) && (len >= minLength))
                objFlags &= ~OBJBIT_MINVIOLATION;
            if ((maxLength > 0) && (len > maxLength))
                objFlags |= OBJBIT_MAXVIOLATION;
            else
                objFlags &= ~OBJBIT_MAXVIOLATION;
            if (len < minLength)
                objFlags |= OBJBIT_MINVIOLATION;
            else
                objFlags &= ~OBJBIT_MINVIOLATION;
            return Value();
        } else if (m.message == "_performaction") {
            performAction(true);
            return Value();
        }
        return Other::message(m);
    }

    void Rubberband::MpCaptureAttrsForSnapshot(MpAttrSnapshot &attrs) const {
        Other::MpCaptureAttrsForSnapshot(attrs);
        attrs.emplace_back("anchor1", Value(anchor1));
        attrs.emplace_back("anchor2", Value(anchor2Object()));
        attrs.emplace_back("strength", Value(strength));
        attrs.emplace_back("length", Value(outerThreshold));
        attrs.emplace_back("threshold", Value(innerThreshold));
        attrs.emplace_back("min", Value(minLength));
        attrs.emplace_back("max", Value(maxLength));
    }

    void Rubberband::MpRestoreAttrsForSnapshot(const MpAttrSnapshot &attrs) {
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
            } else if (entry.first == "strength" || entry.first == "length" ||
                       entry.first == "threshold" || entry.first == "min" ||
                       entry.first == "max") {
                setAttr(entry.first, entry.second);
            } else {
                remaining.push_back(entry);
            }
        }

        Other::MpRestoreAttrsForSnapshot(remaining);

        if (have_anchor1 || have_anchor2) {
            Object *new_anchor1 = have_anchor1 ? (Object *)anchor1_value : (Object *)anchor1;
            Object *new_anchor2 = have_anchor2 ? (Object *)anchor2_value : anchor2Object();
            restoreAnchors(new_anchor1, new_anchor2);
        }
    }

    void Rubberband::MpCaptureSemanticState(MpSemanticState &semantic) const {
        semantic.logical_state = MpCaptureStateForSnapshot();
        semantic.flags = MpCaptureFlagsForSnapshot();
        semantic.fields.clear();
        semantic.refs.clear();

        semantic.fields.emplace_back("strength", Value(strength));
        semantic.fields.emplace_back("length", Value(outerThreshold));
        semantic.fields.emplace_back("threshold", Value(innerThreshold));
        semantic.fields.emplace_back("min", Value(minLength));
        semantic.fields.emplace_back("max", Value(maxLength));

        Object::MpObjectRef ref;
        if (capture_rubber_anchor_ref(anchor1, ref))
            semantic.refs.emplace_back("anchor1", ref);
        if (capture_rubber_anchor_ref(anchor2Object(), ref))
            semantic.refs.emplace_back("anchor2", ref);
    }

    bool Rubberband::MpApplySemanticState(const MpSemanticState &semantic, MpApplyContext ctx) {
        (void)ctx;
        Object *resolved_anchor1 = NULL;
        Object *resolved_anchor2 = NULL;
        bool have_anchor1 = false;
        bool have_anchor2 = false;
        MpAttrSnapshot attrs;
        attrs.reserve(semantic.fields.size() + 2);
        for (const auto &field : semantic.fields)
            attrs.push_back(field);
        for (const auto &entry : semantic.refs) {
            if (entry.first == "anchor1") {
                resolved_anchor1 = resolve_rubber_anchor_ref(entry.second);
                have_anchor1 = dynamic_cast<Actor *>(resolved_anchor1) != NULL;
            } else if (entry.first == "anchor2") {
                resolved_anchor2 = resolve_rubber_anchor_ref(entry.second);
                have_anchor2 = resolved_anchor2 != NULL &&
                               (dynamic_cast<Actor *>(resolved_anchor2) != NULL ||
                                dynamic_cast<Stone *>(resolved_anchor2) != NULL);
            }
        }
        if (!have_anchor1 || !have_anchor2)
            return false;

        attrs.emplace_back("anchor1", Value(resolved_anchor1));
        attrs.emplace_back("anchor2", Value(resolved_anchor2));
        MpRestoreAttrsForSnapshot(attrs);
        MpRestoreFlagsForSnapshot(semantic.flags);
        return true;
    }

    bool Rubberband::MpNeedsSemanticWorldResync() const {
        return true;
    }

    void Rubberband::postAddition() {
        ASSERT(anchor1 != NULL, XLevelRuntime, "Rubberband: 'anchor1' is no actor");
        ASSERT(anchor2.ac != NULL, XLevelRuntime, "Rubberband: 'anchor2' is neither actor nor stone");
        // If the length value is negative (magic value -1.0) the we use the
        // current distance between the two anchors as the rubberband length.
        // Otherwise the length given by the corresponding "length" attribute is used.
        if (outerThreshold == -1.0) {
            outerThreshold = length(posAnchor2() - anchor1->get_pos());
            enigma::Log << "Created rubberband with a length of: " << outerThreshold << "\n";
        }
        model = display::AddRubber(anchor1->get_pos(), posAnchor2(), 240, 140, 20, true);  // orange
        SendMessage(this, "_recheck");
    }

    void Rubberband::preRemoval() {
        model.kill();
        switchAnchor(anchor1, NULL, anchor2Object());
        switchAnchor(anchor2Object(), NULL, anchor1);
    }

    void Rubberband::tick(double /*dt*/) {
        model.update_first(anchor1->get_pos());
        model.update_second(posAnchor2());
    }

    void Rubberband::applyForces(double dt) {
        const double eps = 0.02;  // epsilon distant limit for contacts
        ecl::V2 v = posAnchor2() - anchor1->get_pos();
        double len = ecl::length(v);
        ecl::V2 force;

        // revalidate pending max/min violations
        if ((objFlags & OBJBIT_MAXVIOLATION) && len <= maxLength)
            objFlags &= ~OBJBIT_MAXVIOLATION;
        if ((objFlags & OBJBIT_MINVIOLATION) && len >= minLength)
            objFlags &= ~OBJBIT_MINVIOLATION;

        if (minLength + eps <= len && (len <= maxLength -eps || maxLength == 0)) {
            // length within the purly force controlled min/max limited region
            if (len == 0) {
                force = ecl::V2();
            } else if (len > outerThreshold) {
                force = v * strength * (len - outerThreshold)/len;
            } else if (len < innerThreshold) {
                force = v * strength * (len - innerThreshold)/len;
            }

            ActorInfo *ai = anchor1->get_actorinfo();
            ai->force += force;
            if (!(objFlags & OBJBIT_STONE)) {
                ai = anchor2.ac->get_actorinfo();
                ai->force -= force;
            }

        } else if (objFlags & OBJBIT_STONE) {
            // min/max handling for stone contected rubberbands
            ActorInfo *ai = anchor1->get_actorinfo();
            ecl::V2 vn = normalize(v);
            bool isMax = (len > maxLength - eps);
            bool isMin = (len < minLength + eps);
            ObjectList rl = anchor1->getAttr("rubbers");
            int numRubbers =rl.size();

            // neutralize other force componentes in rubber direction
            double force1 = vn * ai->force;
            if ((!isMin && (force1 > 0)) || (!isMax && (force1 < 0)))
                force1 = 0;
            ai->force -= force1 * vn;

            double relspeed = ai->vel * vn;   // positive for shrinking dist
            if ((!isMin && (relspeed > 0)) || (!isMax && (relspeed < 0)))
                relspeed = 0;
            force = - (1 + 0.8 / numRubbers) * relspeed * vn / dt * ai->mass;  // damping for inverse friction and multiconnections
//            Log << "Rubber stone force "<< force1 << "  " <<relspeed<< "\n";

            // in case one actor is blocked the length can exceed the limits due to later force corrections
            // in the last timestep - we need to correct possible small errors before they sum up
            if (isMax && (len > maxLength) && (relspeed <= 0) && !(objFlags & OBJBIT_MAXVIOLATION)) {
                double dlen = ecl::Min(len - maxLength, len - (minLength + eps));
                dlen = ecl::Max(0.0, dlen);
                force = (ai->mass * dlen / dt / dt) * vn;
            }
            if (isMin && (len < minLength) && (relspeed >= 0) && !(objFlags & OBJBIT_MINVIOLATION)) {
                double dlen = len - minLength;
                if (maxLength > 0) {
                    dlen = ecl::Max(len - minLength, len - (maxLength - eps));
                    dlen = ecl::Min(0.0, dlen);
                }
                force = (ai->mass * dlen / dt / dt) * vn;
            }

            // eliminate limit violations by moderate forces
            if (isMax && (objFlags & OBJBIT_MAXVIOLATION)) {
                force += server::RubberViolationStrength * vn;
            } else if (isMin && (objFlags & OBJBIT_MINVIOLATION)) {
                force -= server::RubberViolationStrength * vn;
            }

            ai->collforce += force;
        } else {
            // two actors bouncing on min/max limits
            ActorInfo *ai1 = anchor1->get_actorinfo();
            ActorInfo *ai2 = anchor2.ac->get_actorinfo();
            ecl::V2 vn = normalize(v);
            double mass = ai1->mass + ai2->mass;
            bool isMax = (len > maxLength - eps);
            bool isMin = (len < minLength + eps);
            bool isBoth = isMax && isMin;
            if (isBoth) {
                isMax = (len > (maxLength - minLength)/2);
                isMin = !isMax;
            }
            ObjectList rl1 = anchor1->getAttr("rubbers");
            ObjectList rl2 = anchor2.ac->getAttr("rubbers");
            int numRubbers = rl1.size() + rl2.size() - 1;

            // redistribute other force components in rubber direction according
            // to the mass of actors to move the complex but to avoid length change

            // component of other forces in rubber direction
            double force1 = vn * ai1->force;
            double force2 = vn * ai2->force;

            // limit to min/max affected forces
            if ((!isMin && (force1 > 0)) || (!isMax && (force1 < 0)))
                force1 = 0;
            if ((!isMin && (force2 < 0)) || (!isMax && (force2 > 0)))
                force2 = 0;
            ai1->force += (-force1 + (force1 + force2) * (ai1->mass)/mass) * vn;
            ai2->force += (-force2 + (force1 + force2) * (ai2->mass)/mass) * vn;

            // bounce if min/max rules are violated
            double relspeed = vn * (ai2->vel - ai1->vel);  // speed of band extension
            double dmu = 2 * ai1->mass * ai2->mass / (ai1->mass + ai2->mass);

            if ((isMax && (relspeed < 0)) || (isMin && (relspeed >0)))
                relspeed = 0;

            force = (dmu * relspeed / dt) * vn;
            force = force * (0.5 + 0.4 / numRubbers);   // damping for inverse friction and multicollision

            // in case one actor is blocked the length can exceed the limits due to later force corrections
            // in the last timestep - we need to correct possible small errors before they sum up
            if (isMax && (len > maxLength) && (relspeed >= 0) && !(objFlags & OBJBIT_MAXVIOLATION)) {
                double dlen = ecl::Min(len - maxLength, len - (minLength + eps));
                dlen = ecl::Max(0.0, dlen);
                force = (dmu * dlen / dt / dt) * vn;
            }
            if (isMin && (len < minLength) && (relspeed <= 0) && !(objFlags & OBJBIT_MINVIOLATION)) {
                double dlen = len - minLength;
                if (maxLength > 0) {
                    dlen = ecl::Max(len - minLength, len - (maxLength - eps));
                    dlen = ecl::Min(0.0, dlen);
                }
                force = (dmu * dlen / dt / dt) * vn;
            }

            // eliminate limit violations by moderate forces
            if (isMax && (objFlags & OBJBIT_MAXVIOLATION)) {
                force += server::RubberViolationStrength * vn;
            } else if (isMax && (objFlags & OBJBIT_MINVIOLATION)) {
                force -= server::RubberViolationStrength * vn;
            }

//            Log << "Rubber force " << force1 <<  "  " << force2 << "  relspeed  " << relspeed  << " both " << isBoth << "\n";
            ai1->collforce += force;
            ai2->collforce -= force;
        }

    }

    Object * Rubberband::anchor2Object() const {
        return (objFlags & OBJBIT_STONE) ? (Object *)anchor2.st : (Object *)anchor2.ac;
    }

    ecl::V2 Rubberband::posAnchor2() const {
        return (objFlags & OBJBIT_STONE) ? anchor2.st->getOwnerPos().center() : anchor2.ac->get_pos();
    }

    void Rubberband::restoreAnchors(Object *newAnchor1, Object *newAnchor2) {
        Actor *new_anchor1 = dynamic_cast<Actor *>(newAnchor1);
        ASSERT(new_anchor1 != NULL, XLevelRuntime, "Rubberband: 'anchor1' is no actor");
        ASSERT(newAnchor2 != NULL, XLevelRuntime, "Rubberband: 'anchor2' is neither actor nor stone");
        ASSERT(newAnchor2->getObjectType() == Object::ACTOR || newAnchor2->getObjectType() == Object::STONE,
               XLevelRuntime, "Rubberband: 'anchor2' is neither actor nor stone");
        ASSERT(newAnchor1 != newAnchor2, XLevelRuntime, "Rubberband: 'anchor1' is identical to 'anchor2'");

        Object *old_anchor1 = anchor1;
        Object *old_anchor2 = anchor2Object();
        const bool new_anchor2_is_stone = newAnchor2->getObjectType() == Object::STONE;
        const bool old_anchor2_is_stone = (objFlags & OBJBIT_STONE) != 0;
        if (old_anchor1 == new_anchor1 && old_anchor2 == newAnchor2 && old_anchor2_is_stone == new_anchor2_is_stone)
            return;

        switchAnchor(old_anchor1, NULL, old_anchor2);
        switchAnchor(old_anchor2, NULL, old_anchor1);

        anchor1 = new_anchor1;
        if (new_anchor2_is_stone) {
            anchor2.st = dynamic_cast<Stone *>(newAnchor2);
            objFlags |= OBJBIT_STONE;
        } else {
            anchor2.ac = dynamic_cast<Actor *>(newAnchor2);
            objFlags &= ~OBJBIT_STONE;
        }

        switchAnchor(NULL, anchor1, anchor2Object());
        switchAnchor(NULL, anchor2Object(), anchor1);
    }

    void Rubberband::switchAnchor(Object *oldAnchor, Object *newAnchor, Object *otherAnchor) {
        if (oldAnchor != NULL) {
            ObjectList olist = oldAnchor->getAttr("rubbers");
            olist.remove(this);
            oldAnchor->setAttr("rubbers", olist);
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
            ObjectList olist;
            if (otherAnchor != NULL) {
                // check on existing rubberbands between anchors
                olist = newAnchor->getAttr("fellows");
                ObjectList::iterator it = find(olist.begin(), olist.end(), otherAnchor);
                if (it != olist.end()) {
                    // we do not allow two rubberbands between identical anchors!
                    // - the user can't see it
                    // - danger of automatic addition of infinte rubberbands, that cause the engine to stop
                    // - danger of contradicting min, max values
                    olist = newAnchor->getAttr("rubbers");
                    for (it = olist.begin(); it != olist.end(); ++it) {
                        Rubberband *oldRubber = dynamic_cast<Rubberband *>(*it);
                        if (otherAnchor == oldRubber->anchor1 || otherAnchor == oldRubber->anchor2Object()) {
                            KillOther(oldRubber);
                            break;
                        }
                    }
                }

                // add both anchors to each others fellows list
                olist = newAnchor->getAttr("fellows");
                olist.push_back(otherAnchor);
                newAnchor->setAttr("fellows", olist);
                olist = otherAnchor->getAttr("fellows");
                olist.push_back(newAnchor);
                otherAnchor->setAttr("fellows", olist);
            }
            olist = newAnchor->getAttr("rubbers");
            olist.push_back(this);
            newAnchor->setAttr("rubbers", olist);
        }
    }

    BOOT_REGISTER_START
        BootRegister(new Rubberband(), "ot_rubberband");
    BOOT_REGISTER_END

} // namespace enigma
