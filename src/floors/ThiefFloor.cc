/*
 * Copyright (C) 2007 Andreas Lochmann
 * Copyright (C) 2007 Raoul Bourquin
 * Copyright (C) 2009 Ronald Lamprecht
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

#include "floors/ThiefFloor.hh"
#include "errors.hh"
#include "Inventory.hh"
#include "items/BagItem.hh"
#include "items/GlassesItem.hh"
#include "player.hh"
#include "world.hh"
//#include "main.hh"

namespace enigma {
    namespace {

        const char *const kSnapshotVictimIdAttr = "$mp_thief_victim_id";
        const char *const kSnapshotBagAttr = "$mp_thief_bag";
        const char *const kSemanticVictimStableIdField = "$mp_thief_victim_stable_id";

        struct DetachedItemSnapshot {
            std::string kind;
            int state = 0;
            uint32_t flags = 0;
            Object::MpAttrSnapshot attrs;
            std::vector<DetachedItemSnapshot> contents;
        };

        void append_u64(std::string &out, uint64_t value) {
            out += ecl::strf("%llu;", static_cast<unsigned long long>(value));
        }

        bool parse_u64(const std::string &in, size_t &pos, uint64_t &value) {
            const size_t end = in.find(';', pos);
            if (end == std::string::npos)
                return false;
            char *parse_end = nullptr;
            const std::string chunk = in.substr(pos, end - pos);
            value = std::strtoull(chunk.c_str(), &parse_end, 10);
            if (!parse_end || *parse_end != '\0')
                return false;
            pos = end + 1;
            return true;
        }

        void append_string(std::string &out, const std::string &value) {
            append_u64(out, static_cast<uint64_t>(value.size()));
            out += value;
        }

        bool parse_string(const std::string &in, size_t &pos, std::string &value) {
            uint64_t length = 0;
            if (!parse_u64(in, pos, length))
                return false;
            if (pos + length > in.size())
                return false;
            value.assign(in, pos, static_cast<size_t>(length));
            pos += static_cast<size_t>(length);
            return true;
        }

        bool serialize_snapshot_value(const Value &value, std::string &out) {
            switch (value.getType()) {
                case Value::DEFAULT:
                case Value::NIL:
                case Value::BOOL:
                case Value::DOUBLE:
                case Value::STRING:
                    append_string(out, value.to_string());
                    return true;
                case Value::POSITION: {
                    const ecl::V2 pos(value);
                    append_string(out, ecl::strf("%.17g,%.17g", pos[0], pos[1]));
                    return true;
                }
                case Value::GRIDPOS: {
                    const GridPos pos(static_cast<ecl::V2>(value));
                    append_string(out, ecl::strf("%d,%d", pos.x, pos.y));
                    return true;
                }
                default:
                    return false;
            }
        }

        bool deserialize_snapshot_value(Value::Type type, const std::string &payload, Value &value) {
            switch (type) {
                case Value::DEFAULT:
                    value = Value(Value::DEFAULT);
                    return true;
                case Value::NIL:
                    value = Value();
                    return true;
                case Value::BOOL:
                    value = Value(payload == "1" || payload == "true");
                    return true;
                case Value::DOUBLE:
                    value = Value(std::strtod(payload.c_str(), nullptr));
                    return true;
                case Value::STRING:
                    value = Value(payload);
                    return true;
                case Value::POSITION: {
                    double x = 0.0;
                    double y = 0.0;
                    if (std::sscanf(payload.c_str(), "%lf,%lf", &x, &y) != 2)
                        return false;
                    value = Value(ecl::V2(x, y));
                    return true;
                }
                case Value::GRIDPOS: {
                    int x = 0;
                    int y = 0;
                    if (std::sscanf(payload.c_str(), "%d,%d", &x, &y) != 2)
                        return false;
                    value = Value(GridPos(x, y));
                    return true;
                }
                default:
                    return false;
            }
        }

        void capture_detached_item_snapshot(Item *item, DetachedItemSnapshot &snapshot) {
            snapshot.kind = item->getKind();
            snapshot.state = item->MpCaptureStateForSnapshot();
            snapshot.flags = item->MpCaptureFlagsForSnapshot();
            Object::MpAttrSnapshot attrs;
            item->MpCaptureAttrsForSnapshot(attrs);
            snapshot.attrs.clear();
            for (const auto &attr : attrs) {
                switch (attr.second.getType()) {
                    case Value::DEFAULT:
                    case Value::NIL:
                    case Value::BOOL:
                    case Value::DOUBLE:
                    case Value::STRING:
                    case Value::POSITION:
                    case Value::GRIDPOS:
                        snapshot.attrs.push_back(attr);
                        break;
                    default:
                        break;
                }
            }

            snapshot.contents.clear();
            if (BagItem *bag = dynamic_cast<BagItem *>(item)) {
                const std::vector<Item *> &contents = bag->MpContentsForSnapshot();
                snapshot.contents.reserve(contents.size());
                for (Item *child : contents) {
                    if (!child)
                        continue;
                    DetachedItemSnapshot child_snapshot;
                    capture_detached_item_snapshot(child, child_snapshot);
                    snapshot.contents.push_back(std::move(child_snapshot));
                }
            }
        }

        void serialize_detached_item_snapshot(const DetachedItemSnapshot &snapshot, std::string &out) {
            append_string(out, snapshot.kind);
            append_u64(out, static_cast<uint64_t>(snapshot.state));
            append_u64(out, static_cast<uint64_t>(snapshot.flags));
            append_u64(out, static_cast<uint64_t>(snapshot.attrs.size()));
            for (const auto &attr : snapshot.attrs) {
                append_string(out, attr.first);
                append_u64(out, static_cast<uint64_t>(attr.second.getType()));
                serialize_snapshot_value(attr.second, out);
            }
            append_u64(out, static_cast<uint64_t>(snapshot.contents.size()));
            for (const auto &child : snapshot.contents)
                serialize_detached_item_snapshot(child, out);
        }

        bool parse_detached_item_snapshot(const std::string &in, size_t &pos, DetachedItemSnapshot &snapshot) {
            snapshot = DetachedItemSnapshot();
            if (!parse_string(in, pos, snapshot.kind))
                return false;
            uint64_t number = 0;
            if (!parse_u64(in, pos, number))
                return false;
            snapshot.state = static_cast<int>(number);
            if (!parse_u64(in, pos, number))
                return false;
            snapshot.flags = static_cast<uint32_t>(number);
            if (!parse_u64(in, pos, number))
                return false;
            const size_t attr_count = static_cast<size_t>(number);
            snapshot.attrs.reserve(attr_count);
            for (size_t i = 0; i < attr_count; ++i) {
                std::string key;
                if (!parse_string(in, pos, key))
                    return false;
                if (!parse_u64(in, pos, number))
                    return false;
                const Value::Type type = static_cast<Value::Type>(number);
                std::string payload;
                if (!parse_string(in, pos, payload))
                    return false;
                Value value;
                if (!deserialize_snapshot_value(type, payload, value))
                    return false;
                snapshot.attrs.push_back(std::make_pair(key, value));
            }
            if (!parse_u64(in, pos, number))
                return false;
            const size_t child_count = static_cast<size_t>(number);
            snapshot.contents.reserve(child_count);
            for (size_t i = 0; i < child_count; ++i) {
                DetachedItemSnapshot child;
                if (!parse_detached_item_snapshot(in, pos, child))
                    return false;
                snapshot.contents.push_back(std::move(child));
            }
            return true;
        }

        Item *restore_detached_item_snapshot(const DetachedItemSnapshot &snapshot, GridPos owner_pos) {
            Item *item = MakeItem(snapshot.kind.c_str());
            if (!item)
                return nullptr;
            item->MpRestoreFlagsForSnapshot(snapshot.flags);
            item->MpRestoreAttrsForSnapshot(snapshot.attrs);
            if (!item->MpRestoreStateForSnapshot(snapshot.state))
                item->setAttr("state", Value(snapshot.state));
            item->setOwnerPos(owner_pos);

            if (BagItem *bag = dynamic_cast<BagItem *>(item)) {
                for (std::vector<DetachedItemSnapshot>::const_reverse_iterator it =
                         snapshot.contents.rbegin(); it != snapshot.contents.rend(); ++it) {
                    Item *child = restore_detached_item_snapshot(*it, owner_pos);
                    if (child)
                        bag->add_item(child);
                }
            }
            return item;
        }

        std::string serialize_bag_snapshot(BagItem *bag) {
            DetachedItemSnapshot snapshot;
            capture_detached_item_snapshot(bag, snapshot);
            std::string out;
            serialize_detached_item_snapshot(snapshot, out);
            return out;
        }

        BagItem *restore_bag_snapshot(const std::string &encoded, GridPos owner_pos) {
            size_t pos = 0;
            DetachedItemSnapshot snapshot;
            if (!parse_detached_item_snapshot(encoded, pos, snapshot))
                return nullptr;
            if (pos != encoded.size())
                return nullptr;
            return dynamic_cast<BagItem *>(restore_detached_item_snapshot(snapshot, owner_pos));
        }

    }  // namespace

    ThiefFloor::ThiefFloor() : Floor("fl_thief", 4.5, 1.5), victimId (0), bag (NULL) {

    }

    ThiefFloor::~ThiefFloor() {
        if (bag != NULL)
            delete bag;
    }

    std::string ThiefFloor::getClass() const {
        return "fl_thief";
    }

    void ThiefFloor::MpCaptureAttrsForSnapshot(MpAttrSnapshot &attrs) const {
        Floor::MpCaptureAttrsForSnapshot(attrs);
        if (victimId != 0)
            attrs.push_back(std::make_pair(std::string(kSnapshotVictimIdAttr), Value(victimId)));
        if (BagItem *bag_item = dynamic_cast<BagItem *>(bag)) {
            attrs.push_back(std::make_pair(std::string(kSnapshotBagAttr),
                                           Value(serialize_bag_snapshot(bag_item))));
        }
    }

    void ThiefFloor::MpRestoreAttrsForSnapshot(const MpAttrSnapshot &attrs) {
        MpAttrSnapshot filtered;
        filtered.reserve(attrs.size());
        int restored_victim_id = 0;
        std::string encoded_bag;
        for (const auto &attr : attrs) {
            if (attr.first == kSnapshotVictimIdAttr) {
                restored_victim_id = static_cast<int>(attr.second);
                continue;
            }
            if (attr.first == kSnapshotBagAttr) {
                encoded_bag = attr.second.to_string();
                continue;
            }
            filtered.push_back(attr);
        }

        Floor::MpRestoreAttrsForSnapshot(filtered);

        victimId = restored_victim_id;
        if (bag != NULL) {
            delete bag;
            bag = NULL;
        }
        if (!encoded_bag.empty())
            bag = restore_bag_snapshot(encoded_bag, get_pos());
    }

    void ThiefFloor::MpCaptureSemanticState(MpSemanticState &semantic) const {
        semantic.logical_state = state;
        semantic.flags = 0;
        semantic.fields.clear();
        semantic.refs.clear();
        if (Actor *victim = dynamic_cast<Actor *>(Object::getObject(victimId))) {
            semantic.fields.emplace_back(kSemanticVictimStableIdField,
                                         Value(static_cast<double>(victim->stable_id())));
        }
        if (BagItem *bag_item = dynamic_cast<BagItem *>(bag))
            semantic.fields.emplace_back(kSnapshotBagAttr, Value(serialize_bag_snapshot(bag_item)));
    }

    bool ThiefFloor::MpApplySemanticState(const MpSemanticState &semantic, MpApplyContext ctx) {
        (void)ctx;
        int restored_victim_id = 0;
        std::string encoded_bag;
        for (const auto &field : semantic.fields) {
            if (field.first == kSemanticVictimStableIdField) {
                const unsigned stable_id = static_cast<unsigned>(static_cast<int>(field.second));
                if (Actor *victim = FindActorByStableId(stable_id))
                    restored_victim_id = victim->getId();
            } else if (field.first == kSnapshotBagAttr) {
                encoded_bag = field.second.to_string();
            }
        }

        Item *restored_bag = NULL;
        if (!encoded_bag.empty()) {
            restored_bag = restore_bag_snapshot(encoded_bag, get_pos());
            if (!restored_bag)
                return false;
        }

        state = semantic.logical_state;
        victimId = restored_victim_id;
        if (bag != NULL)
            delete bag;
        bag = restored_bag;
        init_model();
        return true;
    }

    bool ThiefFloor::MpNeedsSemanticWorldResync() const {
        return true;
    }

    Value ThiefFloor::message(const Message &m) {
        if (m.message == "_capture" && (state == IDLE || state == DRUNKEN) && isDisplayable()) {
            // add items on grid pos that can be picked up to our bag
            Item * it =  GetItem(get_pos());
            if (it != NULL && !(it->get_traits().flags & itf_static) && bag != NULL) {
                dynamic_cast<ItemHolder *>(bag)->add_item(YieldItem(get_pos()));
            }
            // drop bag if pos is not occupied by a static item
            if (GetItem(get_pos()) == NULL) {
                SetItem(get_pos(), bag);
                bag = NULL;
            }
            state = (state == IDLE) ? CAPTURE : DRUNKENCAPTURE;
            init_model();
            return true;
        }
        return Floor::message(m);
    }

    void ThiefFloor::setState(int extState) {
        // block all state writes
    }

    void ThiefFloor::on_creation(GridPos p) {
        objFlags |= (IntegerRand(0, 3) << 24);
        Floor::on_creation(p);
    }

    std::string ThiefFloor::getModelName() const {
        return ecl::strf("fl_thief%d", ((objFlags & OBJBIT_MODEL) >> 24) + 1);
    }

    void ThiefFloor::init_model() {
        std::string basename = getModelName();
        switch (state) {
            case IDLE:
            case CAPTURED:
                set_model(basename);
                break;
            case EMERGING:
                set_anim(basename + "_emerge");
                break;
            case RETREATING:
                set_anim(basename + "_retreat");
                break;
            case CAPTURE:
                set_anim(basename + "_capture");
                break;
            case DRUNKEN:
                set_anim(basename + "_drunken");
                break;
            case DRUNKENCAPTURE:
                set_anim(basename + "_capture");
                break;
        }
    }

    void ThiefFloor::actor_enter(Actor *a) {
        if (state == IDLE && a->is_on_floor()) {
            state = EMERGING;
            victimId = a->getId();
            init_model();
        }
    }

    void ThiefFloor::animcb() {
        switch (state) {
            case EMERGING:
                doSteal();
                if (state != DRUNKEN)
                    state = RETREATING;
                init_model();
                break;
            case RETREATING:
                state = IDLE;
                init_model();
                break;
            case CAPTURE:
            case DRUNKENCAPTURE:
                state = CAPTURED;
                init_model();
                break;
            default:
                ASSERT(0, XLevelRuntime, "ThiefFloor: animcb called with inconsistent state");
        }
    }

    size_t ThiefFloor::MpDebugHiddenBagCount() const {
        if (const BagItem *bag_item = dynamic_cast<const BagItem *>(bag))
            return bag_item->MpContentsForSnapshot().size();
        return 0;
    }

    std::string ThiefFloor::MpDebugHiddenBagFirstKind() const {
        if (const BagItem *bag_item = dynamic_cast<const BagItem *>(bag)) {
            const std::vector<Item *> &contents = bag_item->MpContentsForSnapshot();
            if (!contents.empty() && contents.front())
                return contents.front()->getKind();
        }
        return std::string();
    }

    bool ThiefFloor::MpDebugSetHiddenBagKinds(const std::vector<std::string> &kinds) {
        BagItem *new_bag = NULL;
        if (!kinds.empty()) {
            new_bag = dynamic_cast<BagItem *>(MakeItem("it_bag"));
            if (!new_bag)
                return false;
            new_bag->setOwnerPos(get_pos());
            for (const std::string &kind : kinds) {
                Item *item = MakeItem(kind.c_str());
                if (!item) {
                    delete new_bag;
                    return false;
                }
                item->setOwnerPos(get_pos());
                new_bag->add_item(item);
            }
        }

        if (bag != NULL)
            delete bag;
        bag = new_bag;
        return true;
    }

    void ThiefFloor::doSteal() {
        bool didSteal = false;

        // the actor that hit the thief may no longer exist!
        if (Actor *victim = dynamic_cast<Actor *>(Object::getObject(victimId))) {
            if (Value owner = victim->getAttr("owner")) {
                if (!(victim->has_shield())) {
                    enigma::Inventory *inv = player::GetInventory(owner);
                    if (inv && inv->size() > 0) {
                        if (bag == NULL) {
                            bag = MakeItem("it_bag");
                            bag->setOwnerPos(get_pos());
                        }
                        int i = IntegerRand(0, int (inv->size()-1));
                        Item *it = inv->yield_item(i);
                        dynamic_cast<ItemHolder *>(bag)->add_item(it);
                        didSteal = true;
                        Glasses::updateGlasses();
                        player::RedrawInventory(inv);
                        if (it->getKind() == "it_bottle_idle")
                            state = DRUNKEN;
                    }
                }
            }
        }
        // steal from grid
        if(Item *it = GetItem(get_pos())) {
            if (!(it->get_traits().flags & itf_static)) {
                if (bag == NULL) {
                    bag = MakeItem("it_bag");
                    bag->setOwnerPos(get_pos());
                }
                Item *theit = YieldItem(get_pos());
                dynamic_cast<ItemHolder *>(bag)->add_item(theit);
                didSteal = true;
                if (it->getKind() == "it_bottle_idle")
                    state = DRUNKEN;
            }
        }
        if (didSteal)
            sound_event("thief");
    }

    BOOT_REGISTER_START
        BootRegister(new ThiefFloor(), "fl_thief");
    BOOT_REGISTER_END

} // namespace enigma
