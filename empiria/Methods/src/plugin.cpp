#include "plugin.hpp"

Plugin* pluginInstance;

void init(Plugin* p) {
    pluginInstance = p;
    p->addModel(modelStrata);
    p->addModel(modelCohort);
    p->addModel(modelFactor);
    p->addModel(modelSample);
    p->addModel(modelFrame);
    p->addModel(modelRegress);
    p->addModel(modelTest);
    p->addModel(modelBoot);
    p->addModel(modelLag);
    p->addModel(modelCode);
    p->addModel(modelTab);
    p->addModel(modelSeed);
    p->addModel(modelTape);
    p->addModel(modelGauge);
    p->addModel(modelQuantity);
}
