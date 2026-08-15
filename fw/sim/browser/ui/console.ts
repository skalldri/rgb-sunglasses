/**
 * printk console: the text an extension writes through rgbx_printk, drained
 * once per tick by the sandbox worker and appended here.
 *
 * Scrollback is capped so a chatty per-tick log can't grow the DOM without
 * bound at ~30 ticks/second, and auto-scroll sticks to the bottom only while
 * the user is already there.
 */

const MAX_LINES = 500;

export class ConsolePanel {
  private lines: string[] = [];
  private dirty = false;

  constructor(private readonly pre: HTMLElement) {}

  /** Appends drained log text. Empty strings (the common case) are free. */
  append(text: string): void {
    if (text.length === 0) {
      return;
    }
    for (const line of text.split("\n")) {
      if (line.length > 0) {
        this.lines.push(line);
      }
    }
    this.trim();
  }

  /** Appends a simulator-generated note (activation, fault, source change),
   * visually distinct from extension output. */
  note(text: string): void {
    this.lines.push(`· ${text}`);
    this.trim();
  }

  clear(): void {
    this.lines = [];
    this.dirty = true;
    this.flush();
  }

  /** Rewrites the DOM if anything changed. Called from the render loop so
   * appends stay O(1) even when the extension logs every tick. */
  flush(): void {
    if (!this.dirty) {
      return;
    }
    this.dirty = false;
    const atBottom =
      this.pre.scrollHeight - this.pre.scrollTop - this.pre.clientHeight < 24;
    this.pre.textContent = this.lines.join("\n");
    if (atBottom) {
      this.pre.scrollTop = this.pre.scrollHeight;
    }
  }

  private trim(): void {
    if (this.lines.length > MAX_LINES) {
      this.lines.splice(0, this.lines.length - MAX_LINES);
    }
    this.dirty = true;
  }
}
