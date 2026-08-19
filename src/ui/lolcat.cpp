#include "ui/lolcat.h"

static const int noColors = 30;
static const std::string c_white  =  "\033[0m";
static const std::string colors[] ={"\033[38;5;39m",
																		"\033[38;5;38m",
																		"\033[38;5;44m",
																		"\033[38;5;43m",
																		"\033[38;5;49m",
																		"\033[38;5;48m",
																		"\033[38;5;84m",
																		"\033[38;5;83m",
																		"\033[38;5;119m",
																		"\033[38;5;118m",
																		"\033[38;5;154m",
																		"\033[38;5;148m",
																		"\033[38;5;184m",
																		"\033[38;5;178m",
																		"\033[38;5;214m",
																		"\033[38;5;208m",
																		"\033[38;5;209m",
																		"\033[38;5;203m",
																		"\033[38;5;204m",
																		"\033[38;5;198m",
																		"\033[38;5;199m",
																		"\033[38;5;163m",
																		"\033[38;5;164m",
																		"\033[38;5;128m",
																		"\033[38;5;129m",
																		"\033[38;5;93m",
																		"\033[38;5;99m",
																		"\033[38;5;63m",
																		"\033[38;5;69m",
																		"\033[38;5;33m"};

int mod(const int& x, const int& m) {
	int mod_val;

	if(x >= 0)
		return x%m;
	mod_val = (-x)%m;
	if(mod_val)
		return m-mod_val;
	return 0;
}

int lolcat(std::istream& is, std::ostream& os, size_t width = 7, double gradient = 0.6) {
	std::string inLine;
	size_t cWidth;
	int color,
	    lineNo = 0,
	    r = 29; // rand()%noColors;

	gradient = -std::abs(gradient);

	while(!is.eof()) {
		if(!getline(is, inLine))
			break;

		cWidth = (((lineNo+r)*gradient - std::floor((lineNo+r)*gradient))/1.0)*width;
		color = mod(-int((lineNo+r)*gradient), noColors); 

		size_t i=0;
		os << colors[color];

		while(i < inLine.size()){
			for(size_t I=0; I<width && i<inLine.size(); ++I)
				if(i < inLine.size())
					os << inLine[i++];

			if(width != cWidth)
				cWidth = width;

			color=(color+1)%noColors;
			os << colors[color];
		}


		if(&is == &std::cin)
			os << c_white;

		os << std::endl;
		++lineNo;
	}
	
	os << c_white << std::flush;

	return 0;
}

int lolfilter(std::istream &is, double gradient) {
	return lolcat(is, std::cout, 7, gradient);
}
