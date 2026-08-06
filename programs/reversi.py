# SPDX-License-Identifier: GPL-2.0-only

import tgl
import taudio

BLACK = 1
WHITE = 2
EMPTY = 0
AI_DEPTH = 4

CELL = 62
MARGIN = 16
HUD = 44
BOARD_SIZE = CELL * 8
WIDTH = MARGIN * 2 + BOARD_SIZE
HEIGHT = HUD + BOARD_SIZE + 46

WEIGHTS = (
    120, -20, 20, 5, 5, 20, -20, 120,
    -20, -40, -5, -5, -5, -5, -40, -20,
    20, -5, 15, 3, 3, 15, -5, 20,
    5, -5, 3, 3, 3, 3, -5, 5,
    5, -5, 3, 3, 3, 3, -5, 5,
    20, -5, 15, 3, 3, 15, -5, 20,
    -20, -40, -5, -5, -5, -5, -40, -20,
    120, -20, 20, 5, 5, 20, -20, 120,
)

DIRECTIONS = (
    (-1, -1), (-1, 0), (-1, 1),
    (0, -1), (0, 1),
    (1, -1), (1, 0), (1, 1),
)


def captures(board, cell, player):
    if board[cell] != EMPTY:
        return []
    opponent = 3 - player
    row, column = divmod(cell, 8)
    result = []
    for row_step, column_step in DIRECTIONS:
        scan = []
        rr = row + row_step
        cc = column + column_step
        while 0 <= rr < 8 and 0 <= cc < 8 and board[rr * 8 + cc] == opponent:
            scan.append(rr * 8 + cc)
            rr += row_step
            cc += column_step
        if scan and 0 <= rr < 8 and 0 <= cc < 8 and board[rr * 8 + cc] == player:
            result.extend(scan)
    return result


def legal_moves(board, player):
    return [cell for cell in range(64) if captures(board, cell, player)]


def play(board, cell, player):
    taken = captures(board, cell, player)
    if not taken:
        return False
    board[cell] = player
    for captured in taken:
        board[captured] = player
    return len(taken)


def score(board):
    return board.count(BLACK), board.count(WHITE)


def evaluate(board):
    value = 0
    for cell in range(64):
        disc = board[cell]
        if disc == WHITE:
            value += WEIGHTS[cell]
        elif disc == BLACK:
            value -= WEIGHTS[cell]
    return value


def search(board, player, depth, alpha, beta):
    moves = legal_moves(board, player)
    if depth == 0:
        return evaluate(board)
    if not moves:
        if not legal_moves(board, 3 - player):
            black, white = score(board)
            return (white - black) * 1000
        return search(board, 3 - player, depth - 1, alpha, beta)

    if player == WHITE:
        best = -2000000
        for cell in moves:
            next_board = list(board)
            play(next_board, cell, player)
            value = search(next_board, BLACK, depth - 1, alpha, beta)
            if value > best:
                best = value
            if best > alpha:
                alpha = best
            if beta <= alpha:
                break
        return best

    best = 2000000
    for cell in moves:
        next_board = list(board)
        play(next_board, cell, player)
        value = search(next_board, WHITE, depth - 1, alpha, beta)
        if value < best:
            best = value
        if best < beta:
            beta = best
        if beta <= alpha:
            break
    return best


def ai_move(board):
    best_cell = -1
    best_value = -2000000
    for cell in legal_moves(board, WHITE):
        next_board = list(board)
        play(next_board, cell, WHITE)
        value = search(next_board, BLACK, AI_DEPTH - 1, -2000000, 2000000)
        if best_cell < 0 or value > best_value:
            best_cell = cell
            best_value = value
    return best_cell


def tone(frequency, duration, volume):
    try:
        taudio.tone(frequency, duration, volume)
    except OSError:
        pass


def move_sound(player, flipped):
    if player == BLACK:
        tone(262, 45, 55)
        tone(330 + flipped * 12, 55, 45)
    else:
        tone(392, 45, 48)
        tone(330 - flipped * 8, 55, 42)


def draw(surface, board, player, cursor, last_move, two_player, game_over):
    surface.vgradient(0, 0, WIDTH, HEIGHT, 0x0B1E14, 0x0E2A1C)
    board_x = MARGIN
    board_y = HUD
    surface.fill(board_x - 5, board_y - 5, BOARD_SIZE + 10, BOARD_SIZE + 10, 0x0A3320)
    moves = legal_moves(board, player)
    for cell in range(64):
        row = cell // 8
        column = cell % 8
        x = board_x + column * CELL
        y = board_y + row * CELL
        color = 0x1B7A44 if (row ^ column) & 1 else 0x1E8A4C
        surface.fill(x, y, CELL, CELL, color)
        surface.blend(x, y, CELL, 1, 0x000000, 60)
        surface.blend(x, y, 1, CELL, 0x000000, 60)
        disc = board[cell]
        if disc:
            color = 0x1A1D24 if disc == BLACK else 0xF2F4F8
            surface.disc(x + CELL // 2, y + CELL // 2, CELL // 2 - 6, color)
            shine = 0x50586A if disc == BLACK else 0xFFFFFF
            surface.disc(x + CELL // 2 - 4, y + CELL // 2 - 5, 3, shine)
        elif not game_over and cell in moves and (two_player or player == BLACK):
            surface.disc(x + CELL // 2, y + CELL // 2, 5, 0x8CF0B0)
        if cell == last_move:
            surface.blend(x + 2, y + 2, CELL - 4, 2, 0xFFE060, 160)
    if not game_over and (two_player or player == BLACK):
        x = board_x + (cursor % 8) * CELL
        y = board_y + (cursor // 8) * CELL
        surface.fill(x, y, CELL, 3, 0xFFE060)
        surface.fill(x, y + CELL - 3, CELL, 3, 0xFFE060)
        surface.fill(x, y, 3, CELL, 0xFFE060)
        surface.fill(x + CELL - 3, y, 3, CELL, 0xFFE060)
    black, white = score(board)
    surface.text(MARGIN, 12, "REVERSI", 0xEAF2FF, 3)
    surface.disc(WIDTH - 150, 20, 9, 0x1A1D24)
    surface.disc(WIDTH - 92, 20, 9, 0xF2F4F8)
    surface.text(WIDTH - 136, 12, str(black), 0xEAF2FF, 2)
    surface.text(WIDTH - 78, 12, str(white), 0xEAF2FF, 2)
    surface.text(220, 12, "2 PLAYERS" if two_player else "VS CPU", 0x9CC7AC, 1)
    status = "BLACK TO MOVE"
    if player == WHITE:
        status = "WHITE TO MOVE" if two_player else "WHITE THINKING..."
    surface.text(MARGIN, HUD + BOARD_SIZE + 10, status, 0x9CC7AC, 1)
    surface.text(MARGIN, HUD + BOARD_SIZE + 26,
                 "arrows/wasd MOVE  space PLACE  M MODE  N NEW  Q QUIT", 0x6E86AC, 1)
    if game_over:
        surface.blend(0, 0, WIDTH, HEIGHT, 0x000000, 150)
        if black > white:
            result = "BLACK WINS" if two_player else "YOU WIN!"
        elif white > black:
            result = "WHITE WINS"
        else:
            result = "A DRAW"
        result_x = WIDTH // 2 - len(result) * 12
        surface.text(result_x, HEIGHT // 2 - 30, result,
                     0x60F0B0 if black > white else 0xFF8080, 3)
        surface.text(WIDTH // 2 - 104, HEIGHT // 2 + 14,
                     "press N for a new game", 0xEAF2FF, 1)


def new_board():
    board = [EMPTY] * 64
    board[27] = WHITE
    board[28] = BLACK
    board[35] = BLACK
    board[36] = WHITE
    return board


def game():
    screen_width, screen_height = tgl.screen_size()
    surface = tgl.Surface(WIDTH, HEIGHT)
    origin_x = (screen_width - WIDTH) // 2
    origin_y = (screen_height - HEIGHT) // 2
    if origin_x < 0:
        origin_x = 0
    if origin_y < 0:
        origin_y = 0
    board = new_board()
    player = BLACK
    cursor = 26
    last_move = -1
    two_player = False
    game_over = False
    if taudio.ready():
        tone(262, 70, 45)
        tone(392, 70, 45)
        tone(523, 90, 50)

    try:
        while True:
            black_moves = legal_moves(board, BLACK)
            white_moves = legal_moves(board, WHITE)
            if not black_moves and not white_moves and not game_over:
                game_over = True
                black, white = score(board)
                if black == white:
                    tone(392, 120, 55)
                    tone(392, 120, 55)
                elif (black > white and not two_player) or white > black:
                    tone(523, 100, 55)
                    tone(659, 100, 55)
                    tone(784, 180, 60)
                else:
                    tone(392, 130, 50)
                    tone(330, 130, 50)
                    tone(262, 220, 55)
            elif not game_over and player == BLACK and not black_moves:
                player = WHITE
                tone(196, 140, 45)
            elif not game_over and player == WHITE and not white_moves:
                player = BLACK
                tone(196, 140, 45)

            if not game_over and not two_player and player == WHITE:
                cell = ai_move(board)
                if cell >= 0:
                    flipped = play(board, cell, WHITE)
                    last_move = cell
                    move_sound(WHITE, flipped)
                player = BLACK

            draw(surface, board, player, cursor, last_move, two_player, game_over)
            key = surface.present(origin_x, origin_y)
            if key == ord("q") or key == ord("Q") or key == tgl.KEY_ESC:
                break
            if key == ord("n") or key == ord("N"):
                board = new_board()
                player = BLACK
                cursor = 26
                last_move = -1
                game_over = False
                tone(523, 80, 45)
                continue
            if key == ord("m") or key == ord("M"):
                two_player = not two_player
                board = new_board()
                player = BLACK
                cursor = 26
                last_move = -1
                game_over = False
                tone(330 if two_player else 440, 100, 45)
                continue
            if game_over or (not two_player and player == WHITE):
                continue
            if key == tgl.KEY_UP or key == ord("w") or key == ord("W"):
                if cursor >= 8:
                    cursor -= 8
            elif key == tgl.KEY_DOWN or key == ord("s") or key == ord("S"):
                if cursor < 56:
                    cursor += 8
            elif key == tgl.KEY_LEFT or key == ord("a") or key == ord("A"):
                if cursor % 8:
                    cursor -= 1
            elif key == tgl.KEY_RIGHT or key == ord("d") or key == ord("D"):
                if cursor % 8 != 7:
                    cursor += 1
            elif key == 32 or key == 10 or key == 13:
                moved_player = player
                flipped = play(board, cursor, player)
                if flipped:
                    last_move = cursor
                    player = 3 - player
                    move_sound(moved_player, flipped)
                else:
                    tone(110, 80, 35)
    finally:
        surface.close()


if __name__ == "__main__":
    game()