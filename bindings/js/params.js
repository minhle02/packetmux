// Typed stream params: a kind plus declared fields, carried as JSON inside opaque bytes.
// TypeScript types do not exist at runtime, so the fields are given as values.
import { InvalidParams } from './errors.js';

const TYPE_OF = Symbol('packetmux.streamType');
const MAX_INT = Number.MAX_SAFE_INTEGER; // the largest whole number every language reads exactly
const TYPE_NAMES = new Set(['string', 'int', 'float', 'bool']);
// fatal: invalid UTF-8 is refused. ignoreBOM: a byte-order mark stays, and JSON then refuses it.
const decoder = new TextDecoder('utf-8', { fatal: true, ignoreBOM: true });
const encoder = new TextEncoder();

function parseField(name, spec) {
  const isPlain = typeof spec === 'string';
  const type = isPlain ? spec : spec.type;
  const field = {
    name,
    type: type.replace(/\?$/, ''),
    nullable: type.endsWith('?'),
    hasDefault: !isPlain && 'default' in spec,
    default: isPlain ? undefined : spec.default,
  };
  if (!TYPE_NAMES.has(field.type)) {
    throw new TypeError(`${name}: use string, int, float or bool, each optionally with ?`);
  }
  return field;
}

/** Declares a stream type. The result builds params: TcpForward({ host, port }). */
export function defineStream(kind, fields) {
  const declared = Object.entries(fields).map(([name, spec]) => parseField(name, spec));
  const type = (value) => ({ [TYPE_OF]: type, value });
  type.kind = kind;
  type.fields = declared;
  return type;
}

export const typeOf = (value) => value?.[TYPE_OF];
export const isStreamType = (value) => typeof value === 'function' && 'fields' in value;

function checkValue(field, value) {
  if (value === null || value === undefined) {
    if (field.nullable) {
      return null;
    }
    throw new InvalidParams(`${field.name} is null`);
  }
  const fits = {
    string: () => typeof value === 'string',
    bool: () => typeof value === 'boolean',
    float: () => Number.isFinite(value),
    int: () => Number.isInteger(value) && Math.abs(value) <= MAX_INT,
  }[field.type];
  if (typeof value !== 'bigint' && fits()) {
    return value;
  }
  throw new InvalidParams(`${field.name} is not a ${field.type}`);
}

export function encodeParams(params) {
  const fields = {};
  for (const field of params[TYPE_OF].fields) {
    fields[field.name] = field.name in params.value || !field.hasDefault
      ? checkValue(field, params.value[field.name])
      : field.default;
  }
  return encoder.encode(JSON.stringify(fields));
}

export function decodeParams(type, data) {
  let parsed;
  try {
    parsed = JSON.parse(decoder.decode(data));
  } catch (error) {
    throw new InvalidParams(error.message);
  }
  if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) {
    throw new InvalidParams('params are not an object');
  }
  const values = {};
  for (const field of type.fields) { // undeclared keys are ignored
    if (Object.hasOwn(parsed, field.name)) {
      values[field.name] = checkValue(field, parsed[field.name]);
    } else if (field.hasDefault) {
      values[field.name] = field.default;
    } else {
      throw new InvalidParams(`missing field ${field.name}`);
    }
  }
  return values;
}
