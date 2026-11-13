#include "plugin.hpp"

Plugin* pluginInstance;

void init(Plugin* p) {
	pluginInstance = p;
	p->addModel(modelEscarpo);
	p->addModel(modelLillebror);
	p->addModel(modelZenon);
	p->addModel(modelMorel);
}
