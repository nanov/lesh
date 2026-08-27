#include "leshnici/leshnici.h"

#include "leshnici/prompt/modules.h"
#include "ui/prompt/prompt.h"
#include "substrate/assert.h"

namespace lesh::leshnici {

void install_prompt_modules(ui::prompt::engine& which) {
	// The same door a binding's module comes through, and the same one the
	// built-ins come through inside the engine's constructor. `git` is a name in
	// the registry like any other; nothing here is a side entrance.
	const std::int32_t added = which.register_module("git", &prompt::kModuleGit);
	LESH_ASSERT(added == LESH_OK);
	(void)added;
}

} // namespace lesh::leshnici
