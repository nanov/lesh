#pragma once

// ---------------------------------------------------------------------------
// The built-in modules
//
// Every one is a pure function of `state` and of its parsed params, `constexpr`
// throughout - which is the whole membership test, and why `git` is not one of
// them - so the compiled default renders at compile time and the running shell
// calls exactly the same code.
//
// THE OMISSION RULE IS EACH MODULE'S OWN, and it is checked BEFORE any bytes are
// written. `status` omits on zero, `jobs` omits on none, `duration` omits under
// its floor. That is what makes a placement able to vanish its own affixes: the
// module said nothing at all, so there is nothing to unsay.
//
// THE TYPE-SLOT SPELLINGS ARE prmt's, which is the leading example §6.10 names.
// A one-letter alias beside each long one, because a prompt string is typed by
// hand and `{path:cyan:s}` is what a person actually writes.
// ---------------------------------------------------------------------------
//
// ONE FILE PER MODULE, and this one is the table. `git` is NOT here: it moved
// to `src/leshnici/` with the reader it needs, and an engine only knows it once
// the wiring site has installed the shipped extension set. A template naming it
// on an engine without leshnici is refused as an unknown module, like any other.

#include "leshper/prompt/module.h"
#include "leshper/prompt/module_duration.h"
#include "leshper/prompt/module_env.h"
#include "leshper/prompt/module_jobs.h"
#include "leshper/prompt/module_mode.h"
#include "leshper/prompt/module_path.h"
#include "leshper/prompt/module_status.h"
#include "leshper/prompt/module_time.h"

#include <string_view>

namespace lesh::leshper::prompt {

// --- the singletons and the table ------------------------------------------

// ONE OBJECT PER MODULE, FOR THE WHOLE PROCESS. §6.10's "modules are singletons
// in the registry with free placement" is not a convention here, it is the
// storage: there is exactly one `path`, every `{path…}` in every surface points
// at it, and the memo can therefore compare identity rather than names.
// Each module's own header declares its one object, beside the class it is an
// instance of; this file gathers them into the table.

struct builtin_module {
	std::string_view name;
	const module* which;
};

// The seven `engine()` registers, as a compile-time table. It is a SECOND
// statement of the constructor's list, and deliberately so: the constructor is
// the live registry, this is what a template compiled inside a `static_assert`
// is allowed to assume, and a `constexpr` walk cannot consult a `std::map`. The
// two agreeing is asserted at runtime (`TheValidatorsBuiltInTableIsTheRegistrys`).
inline constexpr builtin_module kBuiltinModules[] = {
	{"duration", &kModuleDuration},
	{"env", &kModuleEnv},
	{"jobs", &kModuleJobs},
	{"mode", &kModuleMode},
	{"path", &kModulePath},
	{"status", &kModuleStatus},
	{"time", &kModuleTime},
};

[[nodiscard]] constexpr const module* builtin_module_named(std::string_view name) noexcept {
	for (const builtin_module& one : kBuiltinModules)
		if (one.name == name)
			return one.which;
	return nullptr;
}

} // namespace lesh::leshper::prompt
