#include <gtest/gtest.h>
#include "legacy/shell_state.h"
#include "legacy/zsh_parser_plus.h"

// fixture
class LeshStateTests : public::testing::Test {
protected:
	static char* envp[];
	lesh_state state;
	LeshStateTests(): state("users/lesh", envp){}
};

char* LeshStateTests::envp[] = {const_cast<char*>("HOME=users/lesh"), const_cast<char*>("PATH=/bin"), const_cast<char*>("ENV=VAR"), nullptr };

TEST_F(LeshStateTests, HomeDirExtraction) {
	EXPECT_EQ("users/lesh", state.home());
}

TEST_F(LeshStateTests, EnvVarExtraction) {
	std::string_view envv;
	auto there = state.try_get_env("ENV", envv);
	EXPECT_EQ(there, true);
	EXPECT_EQ(envv, "VAR");
}

