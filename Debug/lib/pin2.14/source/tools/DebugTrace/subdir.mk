################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
CPP_SRCS += \
../lib/pin2.14/source/tools/DebugTrace/debugtrace.cpp 

OBJS += \
./lib/pin2.14/source/tools/DebugTrace/debugtrace.o 

CPP_DEPS += \
./lib/pin2.14/source/tools/DebugTrace/debugtrace.d 


# Each subdirectory must supply rules for building sources it contributes
lib/pin2.14/source/tools/DebugTrace/%.o: ../lib/pin2.14/source/tools/DebugTrace/%.cpp
	@echo 'Building file: $<'
	@echo 'Invoking: GCC C++ Compiler'
	g++ -O0 -g3 -Wall -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


