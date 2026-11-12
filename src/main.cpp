#include "core/Runtime.hpp"

#include <pl/Mod.hpp>

namespace {

class BlockEntitySelectorMod {
public:
    static BlockEntitySelectorMod& instance() {
        static BlockEntitySelectorMod mod;
        return mod;
    }

    bool load(pl::mod::ModContext& context) { return bes::core::Runtime::get().load(context); }
    bool enable(pl::mod::ModContext& context) { return bes::core::Runtime::get().enable(context); }
    bool disable(pl::mod::ModContext& context) { return bes::core::Runtime::get().disable(context); }
    bool unload(pl::mod::ModContext& context) { return bes::core::Runtime::get().unload(context); }
};

} // namespace

PL_REGISTER_MOD(BlockEntitySelectorMod, BlockEntitySelectorMod::instance())
