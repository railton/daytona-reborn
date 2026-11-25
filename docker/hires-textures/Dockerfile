# The AI upscaling of scripts/hires_textures.py (PyTorch with CUDA 12.8, for
# GeForce RTX 50 and older cards; spandrel loads the models).
#   docker build -t daytona-hires docker/hires-textures
#   docker run --rm --gpus all -u $(id -u):$(id -g) -e HOME=/tmp -v "$PWD:/src" -v CAPTURE:/capture \
#     -v MODELS:/models -w /src daytona-hires python3 scripts/hires_textures.py upscale /capture /models
FROM pytorch/pytorch:2.7.1-cuda12.8-cudnn9-runtime
RUN pip install --no-cache-dir spandrel==0.4.1 safetensors==0.5.3 pillow==11.2.1
