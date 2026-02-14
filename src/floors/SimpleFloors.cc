/*
 * Copyright (C) 2002,2003,2004 Daniel Heck
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

#include "floors/SimpleFloors.hh"

//#include "errors.hh"
#include "input.hh"
#include "main.hh"
#include "player.hh"
#include "world.hh"

namespace enigma {
    
/* -------------------- Abyss -------------------- */

    Abyss::Abyss() : Floor ("fl_abyss", 0.0, 0.0, flf_indestructible) {
    }
    
    std::string Abyss::getClass() const {
        return "fl_abyss";
    }
    
    bool Abyss::is_destructible() const {
        return false;
    }

    void Abyss::actor_contact (Actor* a) {
        SendMessage(a, "_fall");
    }
    
/* -------------------- DummyFloor -------------------- */

    DummyFloor::DummyFloor() : Floor ("fl_dummy", 4.0, 2.5) {
    }
    
    std::string DummyFloor::getClass() const {
        return "fl_dummy";
    }

    void DummyFloor::actor_contact(Actor *) {
        int code = getAttr("code");
        Log << ecl::strf("Entering floor 0x%x\n", code);
    }

/* -------------------- FakeFloor -------------------- */

    FakeFloor::FakeFloor(std::string flavor) : Floor ("fl_fake") {
        setAttr("flavor", flavor);
    }
    
    std::string FakeFloor::getClass() const {
        return "fl_fake";
    }
    
    std::string FakeFloor::getModelName() const {
        return "fl_fake_" + getAttr("flavor").to_string();
    }
    
/* -------------------- Ice -------------------- */

    IceFloor::IceFloor() : Floor ("fl_ice") {
    }
    
    std::string IceFloor::getClass() const {
        return "fl_ice";
    }
    
/* -------------------- InverseFloor -------------------- */

    InverseFloor::InverseFloor(std::string flavor) : Floor ("fl_inverse") {
        setAttr("flavor", flavor);
    }
    
    std::string InverseFloor::getClass() const {
        return "fl_inverse";
    }
    
    std::string InverseFloor::getModelName() const {
        return "fl_inverse_" + getAttr("flavor").to_string();
    }

/* -------------------- Space -------------------- */

    
    Space::Space(bool withForce) : Floor("fl_space") {
        if (withForce)
            objFlags |= OBJBIT_FORCETYP;
    }
    
    std::string Space::getClass() const {
        return "fl_space";
    }
    
    void Space::add_force (Actor *a, ecl::V2 &f) {
        if (objFlags & OBJBIT_FORCETYP)
            f[1] -= server::FlatForce;
        Floor::add_force(a, f);
    }
    
/* -------------------- Swamp -------------------- */

    Swamp::Swamp() : Floor("fl_swamp", 0.0, 0.0, flf_indestructible) {
    }
    
    std::string Swamp::getClass() const {
        return "fl_swamp";
    }
    
    bool Swamp::is_destructible() const {
        return false;
    }

    void Swamp::get_sink_speed (double &sink_speed, double &raise_speed) const { 
        sink_speed = server::SwampSinkSpeed;
        raise_speed = 6.0;
    }

/* -------------------- Yinyang -------------------- */

    YinyangFloor::YinyangFloor(int initState, bool isInvisible) : Floor("fl_yinyang", 5.2, 2.0) {
        state = initState;
        if (isInvisible)
            objFlags |= OBJBIT_INVISIBLE;
    }
    
    std::string YinyangFloor::getClass() const {
        return "fl_yinyang";
    }
    
    void YinyangFloor::setAttr(const std::string &key, const Value &val) {
        if (key == "invisible") {
            objFlags = (objFlags & ~OBJBIT_INVISIBLE) | (val.to_bool() ? OBJBIT_INVISIBLE : 0);
            if (isDisplayable())
                init_model();
            return;
        }
        Floor::setAttr(key, val);
    }
    
    Value YinyangFloor::getAttr(const std::string &key) const {
        if (key == "invisible") {
            return (objFlags & OBJBIT_INVISIBLE) != 0;
        }
        return Floor::getAttr(key);
    }
        
    std::string YinyangFloor::getModelName() const {
        return ecl::strf("fl_yinyang_%s%s", (state == YIN) ? "yin" : "yang", 
                objFlags & OBJBIT_INVISIBLE ? "_invisible" : "");
    }
    
    ecl::V2 YinyangFloor::process_mouseforce (Actor *a, ecl::V2 force) {
        // Single-player semantics:
        // - `fl_yinyang` is keyed to the *current player* (changed by it_yinyang).
        //   Any ball on the active yin/yang floor responds to mouseforce.
        //
        // Multiplayer semantics:
        // - The "current player" toggle is not meaningful.
        // - Extra players can "clone" authored black/white marbles; those clones should
        //   still behave like black/white for yin/yang mechanics even if controllers
        //   were redistributed to different session players.
        if (!input::IsNetworked()) {
            (void)a;
            if (player::CurrentPlayer() == state)
                return getAdhesion() * force;
            return ecl::V2();
        }

        int affinity = -1;

        // Prefer explicit actor color when available: BLACK/YIN is 0, WHITE/YANG is 1.
        if (Value c = a->getAttr("color")) {
            int ci = static_cast<int>(c);
            if (ci == YIN || ci == YANG)
                affinity = ci;
        }

        // Fallback: in multiplayer, owner is a player index; parity matches yin/yang grouping.
        if (affinity < 0) {
            if (Value o = a->getAttr("owner")) {
                int oi = static_cast<int>(o);
                if (oi >= 0)
                    affinity = (oi & 1);
            }
        }

        // Last-resort fallback: infer the controlling player from a single-bit controllers mask.
        if (affinity < 0) {
            int ctr = a->get_controllers();
            if (ctr != 0) {
                unsigned mask = static_cast<unsigned>(ctr);
                unsigned idx = 0;
                while ((mask & 1u) == 0u) {
                    mask >>= 1u;
                    ++idx;
                    if (idx >= 31)
                        break;
                }
                affinity = static_cast<int>(idx & 1u);
            }
        }

        if (affinity == state)
            return getAdhesion() * force;
        else
            return ecl::V2();
    }
    
    BOOT_REGISTER_START
        BootRegister(new Abyss(),     "fl_abyss");
        BootRegister(new DummyFloor(),"fl_dummy");
        BootRegister(new FakeFloor(),  "fl_fake");
        BootRegister(new FakeFloor(),  "fl_fake_abyss");
        BootRegister(new FakeFloor("trigger"),  "fl_fake_trigger");
        BootRegister(new IceFloor(),  "fl_ice");
        BootRegister(new InverseFloor(),  "fl_inverse");
        BootRegister(new InverseFloor(),  "fl_inverse_white");
        BootRegister(new InverseFloor("gray"),  "fl_inverse_gray");
        BootRegister(new Space(),     "fl_space");
        BootRegister(new Space(true), "fl_space_force");
        BootRegister(new Swamp(),     "fl_swamp");
        BootRegister(new YinyangFloor(0), "fl_yinyang");
        BootRegister(new YinyangFloor(0), "fl_yinyang_yin");
        BootRegister(new YinyangFloor(0, true), "fl_yinyang_yin_invisible");
        BootRegister(new YinyangFloor(1), "fl_yinyang_yang");
        BootRegister(new YinyangFloor(1, true), "fl_yinyang_yang_invisible");
    BOOT_REGISTER_END

} // namespace enigma
