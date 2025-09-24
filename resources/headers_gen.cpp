#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>

int main(){
    using nlohmann::json;
    std::ifstream dawn_json("dawn.json");
    json x = json::parse(dawn_json);
    for(const auto& v : x){
        std::cout << v << "\n";
        break;
    }
}
