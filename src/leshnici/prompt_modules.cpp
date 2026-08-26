#include "leshnici/prompt_modules.h"

#include "leshper/prompt/prompt.h"
#include "substrate/assert.h"

namespace lesh::leshnici {

void install_prompt_modules(leshper::prompt::engine& which) {
	// The same door a binding's module comes through, and the same one the
	// built-ins come through inside the engine's constructor. `git` is a name in
	// the registry like any other; nothing here is a side entrance.
	const std::int32_t added = which.register_module("git", &kModuleGit);
	LESH_ASSERT(added == LESH_OK);
	(void)added;
}

} // namespace lesh::leshnici
