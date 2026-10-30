@echo off
rem Uncharted 2. Com UC_READBACK_ASYNC e UC_READBACK_SIDEQ, as copias de constantes abaixo de 0x103a148000 ficam na GPU (UC_SCALAR_COPY_GPU_RANGE); sem isso o Drake fica lento no vagao do trem.
rem Uncharted 2. With UC_READBACK_ASYNC and UC_READBACK_SIDEQ, constant copies below 0x103a148000 stay on the GPU (UC_SCALAR_COPY_GPU_RANGE); without it Drake is slow on the train carriage.
call "%~dp0config.bat"
powershell -ExecutionPolicy Bypass -File "%~dp0..\tools\run_uc3.ps1" -Set "UC_EXECUTABLE=big2-ps4_Shipping.elf,%UC_OTIMIZACOES%,UC_READBACK_ASYNC=1,UC_READBACK_SIDEQ=1,UC_SCALAR_COPY_GPU_RANGE=0x103a100000-0x103a148000,UC_COMPUTE_PIPE=1" %*
