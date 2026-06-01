
```
apt install curl zstd -y
curl -fsSL https://ollama.com/install.sh | sh
curl -fsSL https://claude.ai/install.sh | bash
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc && source ~/.bashrc
```

```
ollama launch claude --model qwen3.5:2b
```

### OpenCode
```
curl -fsSL https://opencode.ai/install | bash
```