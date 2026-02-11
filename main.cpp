#include <bits/stdc++.h>
#include "uci.h"
#include "Engine.h"
#include "Search.h"
#include "MoveGeneration.h"
#include "Evaluation.h"
#include "types.h"
int main(){
    std::cout << "Elderviolet 1.0 by Magnus\n";
    Engine engine;
    uci::loop(engine);
    return 0;
}