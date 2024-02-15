#include <iostream>
#include "vulp/actuation/BulletInterface.h"
#include "vulp/actuation/bullet_utils.h"

namespace vulp::actuation {
int main(int argc, char* argv[]){

    std::cout << "Hello from my world!" << std::endl;
    // Start simulator
    eCONNECT_METHOD flag = eCONNECT_GUI_SERVER;
    
    b3RobotSimulatorClientAPI bullet_ =  b3RobotSimulatorClientAPI(); 

    bool is_connected = bullet_.connect(flag);
    if (!is_connected) {
      throw std::runtime_error("Could not connect to the Bullet GUI");
    }
    else {
        std::cout << "Success!" << std::endl;
        while(1){}

    }
  

    return 0;
}
}
