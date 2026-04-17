local json = {}

local function decode_error(position, message)
  error("json decode error at byte " .. tostring(position) .. ": " .. message, 0)
end

local function skip_whitespace(text, position)
  while true do
    local byte = string.byte(text, position)
    if byte == 32 or byte == 9 or byte == 10 or byte == 13 then
      position = position + 1
    else
      return position
    end
  end
end

local parse_value

local function parse_string(text, position)
  if string.byte(text, position) ~= 34 then
    decode_error(position, "expected string")
  end

  position = position + 1
  local parts = {}
  local part_start = position

  while position <= #text do
    local byte = string.byte(text, position)
    if byte == 34 then
      parts[#parts + 1] = string.sub(text, part_start, position - 1)
      return table.concat(parts), position + 1
    end

    if byte == 92 then
      parts[#parts + 1] = string.sub(text, part_start, position - 1)
      local escape = string.sub(text, position + 1, position + 1)
      if escape == "\"" or escape == "\\" or escape == "/" then
        parts[#parts + 1] = escape
      elseif escape == "b" then
        parts[#parts + 1] = "\b"
      elseif escape == "f" then
        parts[#parts + 1] = "\f"
      elseif escape == "n" then
        parts[#parts + 1] = "\n"
      elseif escape == "r" then
        parts[#parts + 1] = "\r"
      elseif escape == "t" then
        parts[#parts + 1] = "\t"
      elseif escape == "u" then
        local hex = string.sub(text, position + 2, position + 5)
        if #hex ~= 4 or not hex:match("^[0-9a-fA-F]+$") then
          decode_error(position, "invalid unicode escape")
        end

        local codepoint = tonumber(hex, 16)
        if codepoint <= 0x7F then
          parts[#parts + 1] = string.char(codepoint)
        elseif codepoint <= 0x7FF then
          parts[#parts + 1] = string.char(
            0xC0 + math.floor(codepoint / 0x40),
            0x80 + (codepoint % 0x40)
          )
        else
          parts[#parts + 1] = string.char(
            0xE0 + math.floor(codepoint / 0x1000),
            0x80 + (math.floor(codepoint / 0x40) % 0x40),
            0x80 + (codepoint % 0x40)
          )
        end
        position = position + 4
      else
        decode_error(position, "invalid escape character")
      end

      position = position + 2
      part_start = position
    else
      if byte < 32 then
        decode_error(position, "control character in string")
      end
      position = position + 1
    end
  end

  decode_error(position, "unterminated string")
end

local function parse_number(text, position)
  local start = position
  local byte = string.byte(text, position)
  if byte == 45 then
    position = position + 1
  end

  byte = string.byte(text, position)
  if byte == 48 then
    position = position + 1
  else
    if byte == nil or byte < 49 or byte > 57 then
      decode_error(position, "invalid number")
    end
    repeat
      position = position + 1
      byte = string.byte(text, position)
    until byte == nil or byte < 48 or byte > 57
  end

  if string.byte(text, position) == 46 then
    position = position + 1
    byte = string.byte(text, position)
    if byte == nil or byte < 48 or byte > 57 then
      decode_error(position, "invalid fractional number")
    end
    repeat
      position = position + 1
      byte = string.byte(text, position)
    until byte == nil or byte < 48 or byte > 57
  end

  byte = string.byte(text, position)
  if byte == 69 or byte == 101 then
    position = position + 1
    byte = string.byte(text, position)
    if byte == 43 or byte == 45 then
      position = position + 1
    end
    byte = string.byte(text, position)
    if byte == nil or byte < 48 or byte > 57 then
      decode_error(position, "invalid exponent")
    end
    repeat
      position = position + 1
      byte = string.byte(text, position)
    until byte == nil or byte < 48 or byte > 57
  end

  local value = tonumber(string.sub(text, start, position - 1))
  if value == nil then
    decode_error(start, "invalid number")
  end
  return value, position
end

local function parse_literal(text, position, literal, value)
  if string.sub(text, position, position + #literal - 1) ~= literal then
    decode_error(position, "expected " .. literal)
  end
  return value, position + #literal
end

local function parse_array(text, position)
  local result = {}
  position = skip_whitespace(text, position + 1)
  if string.byte(text, position) == 93 then
    return result, position + 1
  end

  while true do
    local value
    value, position = parse_value(text, position)
    result[#result + 1] = value
    position = skip_whitespace(text, position)

    local byte = string.byte(text, position)
    if byte == 93 then
      return result, position + 1
    end
    if byte ~= 44 then
      decode_error(position, "expected ',' or ']'")
    end
    position = skip_whitespace(text, position + 1)
  end
end

local function parse_object(text, position)
  local result = {}
  position = skip_whitespace(text, position + 1)
  if string.byte(text, position) == 125 then
    return result, position + 1
  end

  while true do
    local key
    key, position = parse_string(text, position)
    position = skip_whitespace(text, position)
    if string.byte(text, position) ~= 58 then
      decode_error(position, "expected ':'")
    end
    position = skip_whitespace(text, position + 1)

    local value
    value, position = parse_value(text, position)
    result[key] = value
    position = skip_whitespace(text, position)

    local byte = string.byte(text, position)
    if byte == 125 then
      return result, position + 1
    end
    if byte ~= 44 then
      decode_error(position, "expected ',' or '}'")
    end
    position = skip_whitespace(text, position + 1)
  end
end

parse_value = function(text, position)
  position = skip_whitespace(text, position)
  local byte = string.byte(text, position)
  if byte == nil then
    decode_error(position, "unexpected end of input")
  end
  if byte == 34 then
    return parse_string(text, position)
  end
  if byte == 123 then
    return parse_object(text, position)
  end
  if byte == 91 then
    return parse_array(text, position)
  end
  if byte == 45 or (byte >= 48 and byte <= 57) then
    return parse_number(text, position)
  end
  if byte == 116 then
    return parse_literal(text, position, "true", true)
  end
  if byte == 102 then
    return parse_literal(text, position, "false", false)
  end
  if byte == 110 then
    return parse_literal(text, position, "null", nil)
  end
  decode_error(position, "unexpected character")
end

function json.decode(text)
  if type(text) ~= "string" then
    error("json decode expects a string", 2)
  end

  local value, position = parse_value(text, 1)
  position = skip_whitespace(text, position)
  if position <= #text then
    decode_error(position, "trailing data")
  end
  return value
end

return json
