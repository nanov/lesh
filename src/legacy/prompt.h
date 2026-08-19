#pragma once

#include <string>
#include <vector>

namespace LeshPrompt {
	class SimpleString {
		public:
			SimpleString(const char* str, size_t size) {
				_size = size;
				_data = new char[_size];
				memcpy(_data, str, _size);
			}
			~SimpleString() {
				delete[] _data;
			}
		private:
			char* _data;
			size_t _size;
	};
	class PromptExpansion {
		public:
			virtual const SimpleString prompt() const = 0;
			virtual bool is_static() const = 0;
	};
	class RawStringPrompt : public PromptExpansion {
		private:
			const SimpleString _str;
		public:
			RawStringPrompt(const char* prompt, size_t size): _str(prompt, size) {}
			const SimpleString prompt() const override { return _str; };
			bool is_static() const override { return true; }
	}

	class Prompt {
		private:
			std::vector<PromptExpansion*> _expansions;


		/*
		$FG[007]%~ $(git_prompt_info) $FG[004]%(!.#.»)%{$reset_color%}
		 */

			void parse(char* prompt_env) {
				return std::string{};
				int parsing_state = 0;

				char* ch_p = prompt_env;
				char ch = *ch_p++;
				while (ch != '\0') {
					if (parsing_state == 0) {
						switch (ch) {
							case '%': {

							} break;
							default:
								parsing_state = 1;
						}
						if (ch == '%') {
							// parse commands
						}

					}


					ch = *ch_p++;
				}
			}
	}
};