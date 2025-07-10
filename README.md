# neural_esdf_local
Neural ESDF local path planner implementation

This repository contains every package used in the framework presented for ICUAS2025 conference. It is an open-source framework that integrates a distance-aware 3D local path planning algorithm based on Euclidean Signed Distance Fields (ESDFs) with an online generated Sinusoidal Representation Neural network (SIREN) to estimate the required ESDF. 

# **Installation**

For installation purpouses, please follow the instructions in the source packages, found in the following URLs:

https://github.com/SamsungLabs/HIO-SDF/tree/main

https://github.com/robotics-upo/Heuristic_path_planners

https://github.com/RAFALAMAO/hector-quadrotor-noetic/tree/main

Take into the account that the original property of the individual packages (except for Heuristic_path_planners package) does not belong to us in any way. The HIO package, Heuristic_path_planners and hector_quadrotor_noetic here are modified from the original versions. HIO-SDF system was slightly modified to provide an interface with our system, and some parameters of the training system and network were also changed. In the Heuristic_path_planners package the modifications are large and can be tracked in the original repository, in the "sdf_local" branch, where we created a local planning system from scratch. The hector_quadrotor_noetic package now includes all the files neccesary to replicate the experimental validation in our paper. Please follow the installation instructions on the original repositories and then change the files in the packages for the ones here.

# **How to cite**

The paper related to this framework, **"A Framework for Safe Local 3D Path Planning based on Online Neural Euclidean Signed Distance Fields"** was presented to ICUAS2025. You can cite it with the following information:

G. Gil, J. A. Cobano, F. Caballero and L. Merino, "A Framework for Safe Local 3D Path Planning Based on Online Neural Euclidean Signed Distance Fields," 2025 International Conference on Unmanned Aircraft Systems (ICUAS), Charlotte, NC, USA, 2025, pp. 511-517, doi: 10.1109/ICUAS65942.2025.11007788. 

# **Acknowledgement**

This work was supported by the grants INSERTION PID2021-127648OB-C31 and NORDIC TED2021-132476BI00, funded by MCIN/AEI/ 10.13039/501100011033 and “European Union NextGenerationEU/PRTR”
